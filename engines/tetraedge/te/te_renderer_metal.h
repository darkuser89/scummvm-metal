/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TETRAEDGE_TE_RENDERER_METAL_H
#define TETRAEDGE_TE_RENDERER_METAL_H

#include "tetraedge/te/te_renderer.h"
#include "tetraedge/te/te_light.h"
#include "graphics/metal/renderer.h"

namespace Tetraedge {

struct TeMetalLight {
	float position[4], direction[4], ambient[4], diffuse[4], specular[4], attenuation[4], parameters[4];
};

struct TeMetalVertex {
	float position[3], normal[3], uv[2], color[4];
};

struct TeMetalUniforms {
	float modelView[16], projection[16], normal[16], texture[16], projectedTexture[16];
	float globalAmbient[4], ambient[4], diffuse[4], specular[4], emission[4];
	TeMetalLight lights[8];
	float materialParameters[4];
	uint32 lighting, textured, textureMode, projected;
};

class TeRendererMetal : public TeRenderer {
public:
	using TeRenderer::shadowMode;
	TeRendererMetal();
	~TeRendererMetal() override;
	void init(uint width, uint height) override;
	void clearBuffer(Buffer buf) override;
	void colorMask(bool r,bool g,bool b,bool a) override;
	void disableAllLights() override { _lighting = false; }
	void enableAllLights() override { _lighting = true; }
	void disableTexture() override { _textureEnabled = false; }
	void enableTexture() override { _textureEnabled = true; }
	void disableWireFrame() override { _state.wireframe = false; }
	void enableWireFrame() override { _state.wireframe = true; }
	void disableZBuffer() override { _state.depthTest = _state.depthWrite = false; }
	void enableZBuffer() override { _state.depthTest = _state.depthWrite = true; }
	void drawLine(const TeVector3f32 &from,const TeVector3f32 &to) override;
	void loadProjectionMatrix(const TeMatrix4x4 &matrix) override;
	Common::String renderer() override { return "Metal 4"; }
	Common::String vendor() override { return "Apple"; }
	void renderTransparentMeshes() override;
	void reset() override;
	void setClearColor(const TeColor &color) override { _clearColor = color; }
	void setCurrentColor(const TeColor &color) override { _currentColor = color; }
	void setMatrixMode(MatrixMode mode) override { _matrixMode = mode; }
	void setViewport(int x,int y,int w,int h) override;
	void shadowMode(ShadowMode mode) override;
	void applyMaterial(const TeMaterial &material) override;
	void updateGlobalLight() override;
	void updateScreen() override;
	void draw(const TeMetalVertex *vertices,uint count,const uint32 *indices,uint indexCount,TeMesh::Mode mode,uint textureMode);
	void bindTexture(Graphics::Metal::Texture *texture,const TeMatrix4x4 &matrix);
	void releaseTexture(Graphics::Metal::Texture *texture);
	TeMetalLight &light(uint index);
	TeMatrix4x4 modelViewForLights() const { return _loadedMatrices[MM_GL_MODELVIEW]; }
	Graphics::Metal::Renderer &metal() const { return *_metal; }
	void setShadowTexture(Graphics::Metal::Texture *texture,const TeMatrix4x4 &projection);
	void setDepthWrite(bool enabled) { _state.depthWrite = enabled; }

protected:
	void loadMatrixToGL(const TeMatrix4x4 &matrix) override { _loadedMatrices[_matrixMode] = matrix; }

private:
	Graphics::Metal::Renderer *_metal = nullptr;
	Graphics::Metal::Program *_program = nullptr;
	Graphics::Metal::Program *_pointProgram = nullptr;
	Graphics::Metal::DrawState _state;
	TeMetalUniforms _uniforms = {};
	TeMatrix4x4 _loadedMatrices[3];
	Graphics::Metal::Texture *_boundTexture = nullptr, *_shadowTexture = nullptr;
	bool _lighting = false;
	TeMaterial _material;
};

class TeMeshMetal : public TeMesh {
public:
	void draw() override;
	uint32 getTexEnvMode() const override { return _textureMode; }
	Mode getMode() const override { return _mode; }
	void setMode(Mode mode) override { _mode = mode; }
	void setglTexEnvBlend() override { _textureMode = 1; }

private:
	Mode _mode = MeshMode_Points;
	uint32 _textureMode = 0;
};

class TeLightMetal : public TeLight {
public:
	void disable(uint lightno) override;
	void enable(uint lightno) override;
	void draw(TeCamera &camera) override;
	void update(uint lightno) override;
};

class Te3DTextureMetal : public Te3DTexture {
public:
	Te3DTextureMetal();
	~Te3DTextureMetal() override;
	void bind() const override;
	void copyCurrentRender(uint xoffset,uint yoffset,uint x,uint y) override;
	void create() override;
	void destroy() override;
	void forceTexData(uint texture,uint xsize,uint ysize) override;
	bool load(const TeImage &image) override;
	bool unload() override;
	void update(const TeImage &image,uint xoff,uint yoff) override;
	void writeTo(Graphics::Surface &surface) override;

private:
	Graphics::Metal::Texture *_texture = nullptr;
	Graphics::Surface _pixels;
	void updateMatrix(bool flipY);
};

} // End of namespace Tetraedge

#endif
