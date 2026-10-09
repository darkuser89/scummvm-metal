/* ScummVM - Graphic Adventure Engine
	* SPDX-License-Identifier: GPL-3.0-or-later
	*/

#ifndef GRAPHICS_SURFACE_RASTERIZER_H
#define GRAPHICS_SURFACE_RASTERIZER_H

#include "common/noncopyable.h"

#ifdef USE_METAL
#include "graphics/metal/legacy-rasterizer.h"
namespace Graphics { typedef Metal::LegacyRasterizer SurfaceRasterizer; }
#elif defined(USE_TINYGL)
#include "graphics/tinygl/tinygl.h"
#include "graphics/surface.h"
namespace Graphics {
class SurfaceRasterizer : public Common::NonCopyable {
public:
	SurfaceRasterizer() : _context(nullptr), _width(0), _height(0) {}
	~SurfaceRasterizer() { if (_context) TinyGL::destroyContext(_context); }
	bool initialized() const { return _context != nullptr; }
	bool init(int w, int h, const PixelFormat &format) {
	 if (_context) return w == _width && h == _height;
	 _context = TinyGL::createContext(w, h, format, 256, false, false, 1024 * 1024);
	 if (!_context) return false;
	 _width = w; _height = h; makeCurrent(); return true;
	}
	void makeCurrent() { TinyGL::setContext(_context); }
	void getSurfaceRef(Surface &surface) { makeCurrent(); TinyGL::getSurfaceRef(surface); }
	void copyBackground(const Surface &surface) {
	 Surface destination; getSurfaceRef(destination);
	 destination.copyRectToSurface(surface.getPixels(), surface.pitch, 0, 0, surface.w, surface.h);
	}
	void presentBuffer() { makeCurrent(); TinyGL::presentBuffer(); }
	void readInto(Surface &surface) {
	 presentBuffer(); Surface source; getSurfaceRef(source);
	 surface.copyRectToSurface(source.getPixels(), source.pitch, 0, 0, surface.w, surface.h);
	}
	void viewport(int x, int y, int w, int h) { tglViewport(x, y, w, h); }
	void enable(uint cap) { tglEnable(cap); }
	void disable(uint cap) { tglDisable(cap); }
	void clear(uint mask) { tglClear(mask); }
	void clearColor(float r, float g, float b, float a) { tglClearColor(r, g, b, a); }
	void clearDepth(float value) { tglClearDepth(value); }
	void depthFunc(uint func) { tglDepthFunc(func); }
	void depthMask(bool value) { tglDepthMask(value); }
	void frontFace(uint winding) { tglFrontFace(winding); }
	void shadeModel(uint mode) { tglShadeModel(mode); }
	void alphaFunc(uint func, float reference) { tglAlphaFunc(func, reference); }
	void blendFunc(uint src, uint dst) { tglBlendFunc(src, dst); }
	void matrixMode(uint mode) { tglMatrixMode(mode); }
	void loadIdentity() { tglLoadIdentity(); }
	void orthof(float l, float r, float b, float t, float n, float f) { tglOrthof(l, r, b, t, n, f); }
	void frustum(float l, float r, float b, float t, float n, float f) { tglFrustum(l, r, b, t, n, f); }
	void scalef(float x, float y, float z) { tglScalef(x, y, z); }
	void genTextures(int n, uint *ids) { tglGenTextures(n, ids); }
	void deleteTextures(int n, const uint *ids) { tglDeleteTextures(n, ids); }
	void bindTexture(uint target, uint id) { tglBindTexture(target, id); }
	void texParameteri(uint target, uint parameter, int value) { tglTexParameteri(target, parameter, value); }
	void texEnvi(uint target, uint parameter, int value) { tglTexEnvi(target, parameter, value); }
	void texImage2D(uint target, int level, int internalFormat, int w, int h, int border, uint format, uint type, const void *pixels) { tglTexImage2D(target, level, internalFormat, w, h, border, format, type, pixels); }
	void begin(uint primitive) { tglBegin(primitive); }
	void color4ub(byte r, byte g, byte b, byte a) { tglColor4ub(r, g, b, a); }
	void color4f(float r, float g, float b, float a) { tglColor4f(r, g, b, a); }
	void texCoord2f(float u, float v) { tglTexCoord2f(u, v); }
	void normal3f(float x, float y, float z) { tglNormal3f(x, y, z); }
	void vertex3f(float x, float y, float z) { tglVertex3f(x, y, z); }
	void end() { tglEnd(); }
private:
	TinyGL::ContextHandle *_context;
	int _width, _height;
};
} // namespace Graphics
#endif

#endif
