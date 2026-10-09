/* ScummVM - Graphic Adventure Engine
*
* ScummVM is the legal property of its developers, whose names
* are too numerous to list here. Please refer to the COPYRIGHT
* file distributed with this source distribution.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*/

#ifndef WATCHMAKER_OPENGL_RENDERER_H
#define WATCHMAKER_OPENGL_RENDERER_H


#include "watchmaker/3d/math/Matrix4x4.h"
#include "watchmaker/utils.h"
#include "watchmaker/render.h"
#include "watchmaker/3d/render/opengl_3d.h"
#ifdef USE_METAL
#include "graphics/metal/renderer.h"
#endif

namespace Watchmaker {

enum class TransformMatrix {
	PROJECTION,
	VIEW
};

enum class RenderState {
	LIGHT,
	CLIP,
	EXTENT,
	ALPHABLEND,
	ALPHAREF, // TODO
	ZENABLE, // TODO
	ZWRITE_ENABLE, // TODO
	TEXTUREFACTOR // TODO
};

enum class BlendFactor {
	ONE,
	ZERO,
	SRCALPHA,
	INVSRCALPHA,
	INVSRCCOLOR,
	SRCCOLOR,
	DESTCOLOR,
	INVDESTCOLOR
};

enum class TextureWrapMode {
	WRAP,
	CLAMP
};

enum class PrimitiveType {
	LINE,
	TRIANGLE
};

Common::SharedPtr<TextureData> createTextureFromSurface(Graphics::Surface &surface, int texFormat);
Texture *createGLTexture();

class OpenGLRenderer {
public:
#ifdef USE_METAL
	void initMetal(float ratio);
	void setMetalProjection(const float *matrix);
	void bindMetalTexture(Graphics::Metal::Texture *texture) { _metalTexture = texture; }
	void releaseMetalTexture(Graphics::Metal::Texture *texture) { if (_metalTexture == texture) _metalTexture = nullptr; }
	void drawMetalBatch(PrimitiveType primitive, const VertexBuffer &vertices, const uint16 *faces, uint count);
	void drawMetalVertices(PrimitiveType primitive, Vertex *vertices, int count);
	void pushMetalModelView();
	void popMetalModelView();
	void setMetalTransform(TransformMatrix which, const Matrix4x4 &matrix);
	void setMetalState(RenderState state, int value);
	void setMetalBlend(BlendFactor src, BlendFactor dst);
	void setMetalWrap(TextureWrapMode mode);
	void enterMetal2D();
	void exitMetal2D();
#endif
	void enter2Dmode();
	void exit2Dmode();

	void pushModelView();
	void popModelView();
	void setTransformMatrix(TransformMatrix which, const Matrix4x4 &matrix);
	// TODO: This should be split.
	void setRenderState(RenderState state, int value);
	void setBlendFunc(BlendFactor src, BlendFactor dst);
	bool error() const {
		//warning("TODO: Implement error");
		return false;
	}
	Common::String getErrorString() {
		warning("TODO: Implement getErrorString");
		return "";
	}
	// TODO: This just maps to the D3D way to setting textures
	void setTexture(int stage, void *texture);
	void setTexture(int stage, const gTexture &texture);
	void setTextureWrapMode(int index, TextureWrapMode mode);

	void drawPrimitives(PrimitiveType primitiveType, Vertex *vertices, int numPrimitives);
	void drawIndexedPrimitivesVBO(PrimitiveType primitiveType, int VBO, int firstVertex, int numVertices, uint16 *faces, uint32 numFaces);
	void drawIndexedPrimitivesVBO(PrimitiveType primitiveType, Common::SharedPtr<VertexBuffer> VBO, int firstVertex, int numVertices, Common::Array<uint16> faces, uint32 numFaces);
	void drawIndexedPrimitivesVBO(PrimitiveType primitiveType, gBatchBlock &bb);
	bool supportsMultiTexturing() const { // TODO
		return false;
	}

private:
#ifdef USE_METAL
	Matrix4x4 _metalModelView, _metalProjection;
	Common::Array<Matrix4x4> _metalModelStack, _metalProjectionStack;
	Graphics::Metal::DrawState _metalState;
	Graphics::Metal::Texture *_metalTexture = nullptr;
#endif
};

extern OpenGLRenderer *g_renderer;

} // End of namespace Watchmaker

#endif // WATCHMAKER_OPENGL_RENDERER_H
