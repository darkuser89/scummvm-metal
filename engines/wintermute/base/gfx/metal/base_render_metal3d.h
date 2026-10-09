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

#ifndef WINTERMUTE_BASE_RENDER_METAL3D_H
#define WINTERMUTE_BASE_RENDER_METAL3D_H

#include "engines/wintermute/base/gfx/base_renderer3d.h"
#include "engines/wintermute/dctypes.h"

#include "graphics/transform_struct.h"

#if defined(USE_METAL)

#include "engines/wintermute/base/gfx/metal/metal_draw.h"

namespace Wintermute {

class BaseSurfaceMetal3D;

class BaseRenderMetal3D : public BaseRenderer3D, private MetalBackend::Context {
	friend class BaseSurfaceMetal3D;
	friend class Mesh3DSMetal;
	friend class XMeshMetal;
	friend class ShadowVolumeMetal;

	struct SpriteVertex {
		float x;
		float y;
		float z;
		float u;
		float v;
		float r;
		float g;
		float b;
		float a;
	};

	struct RectangleVertex {
		float x;
		float y;
		float z;
	};

	struct SimpleShadowVertex {
		float x;
		float y;
		float z;
		float nx;
		float ny;
		float nz;
		float u;
		float v;
	};


public:
	BaseRenderMetal3D(BaseGame *inGame = nullptr);
	~BaseRenderMetal3D() override;

	bool invalidateTexture(BaseSurface *texture) override;

	bool invalidateDeviceObjects() override;
	bool restoreDeviceObjects() override;

	bool resetDevice() override;

	void setSpriteBlendMode(Graphics::TSpriteBlendMode blendMode, bool forceChange = false) override;

	void setAmbientLightRenderState() override;

	int getMaxActiveLights() override;
	void lightEnable(int index, bool enable) override;
	void setLightParameters(int index, const DXVector3 &position, const DXVector3 &direction, const DXVector4 &diffuse, bool spotlight) override;

	void enableCulling() override;
	void disableCulling() override;

	bool enableShadows() override;
	bool disableShadows() override;
	bool shadowVolumeSupported() override;

	BaseImage *takeScreenshot(int newWidth = 0, int newHeight = 0) override;
	bool fadeToColor(byte r, byte g, byte b, byte a) override;
	bool flip() override;
	bool clear() override;

	bool setViewport(int left, int top, int right, int bottom) override;
	bool drawLine(int x1, int y1, int x2, int y2, uint32 color) override;
	bool fillRect(int x, int y, int w, int h, uint32 color) override;

	DXMatrix *buildMatrix(DXMatrix* out, const DXVector2 *centre, const DXVector2 *scaling, float angle);
	void transformVertices(struct SpriteVertex *vertices, const DXVector2 *centre, const DXVector2 *scaling, float angle);

	bool setProjection() override;
	bool setProjection2D(MetalBackend::Shader *);
	bool setWorldTransform(const DXMatrix &transform) override;
	bool setViewTransform(const DXMatrix &transform) override;
	bool setProjectionTransform(const DXMatrix &transform) override;

	bool initRenderer(int width, int height, bool windowed) override;
	bool setup2D(bool force = false) override;
	bool setup3D(Camera3D *camera, bool force = false) override;

	Common::String getName() const override {
		return "Apple Silicon Metal 3 renderer";
	};
	bool displayDebugInfo() override {
		return STATUS_FAILED;
	};
	bool drawShaderQuad() override {
		return STATUS_FAILED;
	}

	float getScaleRatioX() const override {
		return 1.0f;
	}
	float getScaleRatioY() const override {
		return 1.0f;
	}

	BaseSurface *createSurface() override;

	bool startSpriteBatch() override;
	bool endSpriteBatch() override;
	bool commitSpriteBatch() override;

	bool drawSpriteEx(BaseSurface *texture, const Common::Rect32 &rect, const DXVector2 &pos, const DXVector2 &rot, const DXVector2 &scale,
	                  float angle, uint32 color, bool alphaDisable, Graphics::TSpriteBlendMode blendMode, bool mirrorX, bool mirrorY) override;

	void renderSceneGeometry(const BaseArray<AdWalkplane *> &planes, const BaseArray<AdBlock *> &blocks,
	                         const BaseArray<AdGeneric *> &generics, const BaseArray<Light3D *> &lights, Camera3D *camera) override;
	void renderShadowGeometry(const BaseArray<AdWalkplane *> &planes, const BaseArray<AdBlock *> &blocks, const BaseArray<AdGeneric *> &generics, Camera3D *camera) override;

	Mesh3DS *createMesh3DS() override;
	XMesh *createXMesh() override;
	ShadowVolume *createShadowVolume() override;

	bool setViewport3D(DXViewport *viewport) override;

	void postfilter() override;
	void setPostfilter(PostFilter postFilter) override { _postFilterMode = postFilter; };

	MetalBackend::Shader *_shadowMaskShader;

private:
	bool setupLines();
	void displaySimpleShadow(BaseObject *object) override;

	SimpleShadowVertex _simpleShadow[4];

	Graphics::TSpriteBlendMode _blendMode;

	DXMatrix _clipProjectionMatrix;
	float _alphaRef;

	Common::Array<DXMatrix> _transformStack;

	Math::Vector4d _flatShadowColor;

	uint32 _spriteVBO{};
	uint32 _fadeVBO{};
	uint32 _rectangleVBO{};
	uint32 _simpleShadowVBO{};
	uint32 _postfilterVBO{};
	uint32 _brightnessVBO{};
	MetalBackend::Shader *_spriteShader{};
	MetalBackend::Shader *_fadeShader{};
	MetalBackend::Shader *_xmodelShader{};
	MetalBackend::Shader *_geometryShader{};
	MetalBackend::Shader *_simpleShadowShader{};
	MetalBackend::Shader *_flatShadowShader{};
	MetalBackend::Shader *_shadowVolumeShader{};
	MetalBackend::Shader *_lineShader{};
	MetalBackend::Shader *_postfilterShader{};
	MetalBackend::Shader *_brightnessShaderOknytt{};
	MetalBackend::Shader *_brightnessShaderJulia{};
	uint32 _postfilterTexture;
};

} // namespace Wintermute

#endif // defined(USE_METAL)

#endif
