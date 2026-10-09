/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_LEGACY_RASTERIZER_H
#define GRAPHICS_METAL_LEGACY_RASTERIZER_H

#include "graphics/metal/renderer.h"
#include "graphics/tinygl/gl.h"

namespace Graphics {
namespace Metal {

// Native GPU rasterization for engines that composite actors into a CPU surface.
// The immediate-mode state is retained here; no TinyGL context is created.
class LegacyRasterizer : public Common::NonCopyable {
public:
	LegacyRasterizer();
	~LegacyRasterizer();
	bool init(int width, int height, const PixelFormat &format);
	bool initialized() const { return _initialized; }
	void makeCurrent() {}
	void getSurfaceRef(Surface &surface) { surface = _surface; }
	void copyBackground(const Surface &surface);
	void presentBuffer();
	void readInto(Surface &surface);
	void viewport(int x, int y, int width, int height);
	void enable(uint cap);
	void disable(uint cap);
	void clear(uint mask);
	void clearColor(float r, float g, float b, float a);
	void clearDepth(float value) { _clearDepth = value; }
	void depthFunc(uint func);
	void depthMask(bool enabled) { _state.depthWrite = enabled; }
	void frontFace(uint winding) { _state.clockwise = winding == TGL_CW; }
	void shadeModel(uint mode);
	void alphaFunc(uint func, float reference);
	void blendFunc(uint source, uint destination);
	void matrixMode(uint mode);
	void loadIdentity();
	void orthof(float left, float right, float bottom, float top, float near, float far);
	void frustum(float left, float right, float bottom, float top, float near, float far);
	void scalef(float x, float y, float z);
	void genTextures(int count, uint *ids);
	void deleteTextures(int count, const uint *ids);
	void bindTexture(uint target, uint id);
	void texParameteri(uint target, uint parameter, int value);
	void texEnvi(uint target, uint parameter, int value);
	void texImage2D(uint target, int level, int internalFormat, int width, int height,
	                int border, uint format, uint type, const void *pixels);
	void begin(uint primitive);
	void color4ub(byte r, byte g, byte b, byte a);
	void color4f(float r, float g, float b, float a);
	void texCoord2f(float u, float v);
	void normal3f(float, float, float) {} // Both callers render without lighting.
	void vertex3f(float x, float y, float z);
	void end();

private:
	struct Image { Texture *texture = nullptr; bool allocated = false, linear = false; Wrap wrapS = kClamp, wrapT = kClamp; };
	Renderer _renderer;
	Surface _surface;
	Texture *_background;
	Common::Array<Image> _images;
	Common::Array<Vertex> _vertices;
	Vertex _vertex;
	DrawState _state;
	Uniforms _uniforms;
	float _projection[16], _modelView[16], _clearColor[4];
	float _clearDepth;
	uint _matrixMode, _primitive, _boundImage;
	bool _initialized, _textured, _alphaTest;
	float *matrix();
	void multiply(const float *right);
	void setEnabled(uint cap, bool enabled);
	void uploadBackground();
};

} // namespace Metal
} // namespace Graphics

#endif
