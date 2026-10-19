/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HPL_LOW_LEVEL_GRAPHICS_METAL_H
#define HPL_LOW_LEVEL_GRAPHICS_METAL_H

#include "hpl1/engine/graphics/LowLevelGraphics.h"
#include "hpl1/engine/impl/metal_shader_types.h"
#include "graphics/metal/renderer.h"
#include "common/hashmap.h"

namespace hpl {

class MetalGpuProgram;
class MetalTexture;

class LowLevelGraphicsMetal : public iLowLevelGraphics {
public:
	LowLevelGraphicsMetal();
	~LowLevelGraphicsMetal();

	bool Init(int alWidth, int alHeight, int alBpp, int abFullscreen, int alMultisampling,
			  const tString &asWindowCaption);

	int GetCaps(eGraphicCaps aType) const;

	void ShowCursor(bool abX);

	void SetMultisamplingActive(bool abX);

	void SetGammaCorrection(float afX);
	float GetGammaCorrection();

	int GetMultisampling() { return _multisampling; }

	void SetClipPlane(int alIdx, const cPlanef &aPlane);
	cPlanef GetClipPlane(int alIdx, const cPlanef &aPlane);
	void SetClipPlaneActive(int alIdx, bool abX);

	cVector2f GetScreenSize();
	cVector2f GetVirtualSize();
	void SetVirtualSize(cVector2f avSize);

	Bitmap2D *CreateBitmap2D(const cVector2l &avSize);
	FontData *CreateFontData(const tString &asName);

	iTexture *CreateTexture(bool abUseMipMaps, eTextureType aType, eTextureTarget aTarget);
	iTexture *CreateTexture(const tString &asName, bool abUseMipMaps, eTextureType aType, eTextureTarget aTarget);
	iTexture *CreateTexture(Bitmap2D *apBmp, bool abUseMipMaps, eTextureType aType, eTextureTarget aTarget);
	iTexture *CreateTexture(const cVector2l &avSize, int alBpp, cColor aFillCol,
							bool abUseMipMaps, eTextureType aType, eTextureTarget aTarget);

	Graphics::PixelFormat *GetPixelFormat();

	iGpuProgram *CreateGpuProgram(const tString &vertex, const tString &fragment);

	void SaveScreenToBMP(const tString &asFile);

	/////////// MATRIX METHODS /////////////////////////

	void PushMatrix(eMatrix aMtxType);
	void PopMatrix(eMatrix aMtxType);
	void SetIdentityMatrix(eMatrix aMtxType);

	void SetMatrix(eMatrix aMtxType, const cMatrixf &a_mtxA);

	void TranslateMatrix(eMatrix aMtxType, const cVector3f &avPos);
	void RotateMatrix(eMatrix aMtxType, const cVector3f &avRot);
	void ScaleMatrix(eMatrix aMtxType, const cVector3f &avScale);

	void SetOrthoProjection(const cVector2f &avSize, float afMin, float afMax);

	/////////// DRAWING METHODS /////////////////////////

	// OCCLUSION
	iOcclusionQuery *CreateOcclusionQuery();
	void DestroyOcclusionQuery(iOcclusionQuery *apQuery);

	// CLEARING THE FRAMEBUFFER
	void ClearScreen();

	void SetClearColor(const cColor &aCol);
	void SetClearDepth(float afDepth);
	void SetClearStencil(int alVal);

	void SetClearColorActive(bool abX);
	void SetClearDepthActive(bool abX);
	void SetClearStencilActive(bool abX);

	void SetColorWriteActive(bool abR, bool abG, bool abB, bool abA);
	void SetDepthWriteActive(bool abX);

	void SetCullActive(bool abX);
	void SetCullMode(eCullMode aMode);

	// DEPTH
	void SetDepthTestActive(bool abX);
	void SetDepthTestFunc(eDepthTestFunc aFunc);

	// ALPHA
	void SetAlphaTestActive(bool abX);
	void SetAlphaTestFunc(eAlphaTestFunc aFunc, float afRef);

	// STENCIL
	void SetStencilActive(bool abX);
	/*void SetStencilTwoSideActive(bool abX);
	void SetStencilFace(eStencilFace aFace);
	void SetStencilFunc(eStencilFunc aFunc,int alRef, unsigned int aMask);
	void SetStencilOp(eStencilOp aFailOp,eStencilOp aZFailOp,eStencilOp aZPassOp);*/
	void SetStencil(eStencilFunc aFunc, int alRef, unsigned int aMask,
					eStencilOp aFailOp, eStencilOp aZFailOp, eStencilOp aZPassOp);
	void SetStencilTwoSide(eStencilFunc aFrontFunc, eStencilFunc aBackFunc,
						   int alRef, unsigned int aMask,
						   eStencilOp aFrontFailOp, eStencilOp aFrontZFailOp, eStencilOp aFrontZPassOp,
						   eStencilOp aBackFailOp, eStencilOp aBackZFailOp, eStencilOp aBackZPassOp);
	void SetStencilTwoSide(bool abX);

	// SCISSOR
	void SetScissorActive(bool abX);
	void SetScissorRect(const cRect2l &aRect);

	// BLENDING
	void SetBlendActive(bool abX);
	void SetBlendFunc(eBlendFunc aSrcFactor, eBlendFunc aDestFactor);
	void SetBlendFuncSeparate(eBlendFunc aSrcFactorColor, eBlendFunc aDestFactorColor,
							  eBlendFunc aSrcFactorAlpha, eBlendFunc aDestFactorAlpha);

	// TEXTURE
	void SetTexture(unsigned int alUnit, iTexture *apTex);
	void SetActiveTextureUnit(unsigned int alUnit);
	void SetTextureEnv(eTextureParam aParam, int alVal);
	void SetTextureConstantColor(const cColor &color);

	void SetColor(const cColor &aColor);

	// POLYGONS
	iVertexBuffer *CreateVertexBuffer(tVertexFlag aFlags, eVertexBufferDrawType aDrawType,
									  eVertexBufferUsageType aUsageType,
									  int alReserveVtxSize = 0, int alReserveIdxSize = 0);

	void DrawRect(const cVector2f &avPos, const cVector2f &avSize, float afZ);

	void DrawTri(const tVertexVec &avVtx);
	void DrawTri(const cVertex *avVtx);

	void DrawQuad(const tVertexVec &avVtx);
	void DrawQuad(const tVertexVec &avVtx, const cColor aCol);
	void DrawQuad(const tVertexVec &avVtx, const float afZ);
	void DrawQuad(const tVertexVec &avVtx, const float afZ, const cColor &aCol);
	void DrawQuadMultiTex(const tVertexVec &avVtx, const tVector3fVec &avExtraUvs);

	void AddVertexToBatch(const cVertex &apVtx);
	void AddVertexToBatch(const cVertex *apVtx, const cVector3f *avTransform);
	void AddVertexToBatch(const cVertex *apVtx, const cMatrixf *aMtx);

	void AddVertexToBatch_Size2D(const cVertex *apVtx, const cVector3f *avTransform,
								 const cColor *apCol, const float &mfW, const float &mfH);

	void AddVertexToBatch_Raw(const cVector3f &avPos, const cColor &aColor,
							  const cVector3f &avTex);

	void AddTexCoordToBatch(unsigned int alUnit, const cVector3f *apCoord);
	void SetBatchTextureUnitActive(unsigned int alUnit, bool abActive);

	void AddIndexToBatch(int alIndex);

	void FlushTriBatch(tVtxBatchFlag aTypeFlags, bool abAutoClear = true);
	void FlushQuadBatch(tVtxBatchFlag aTypeFlags, bool abAutoClear = true);
	void ClearBatch();

	// PRIMITIVES
	void DrawLine(const cVector3f &avBegin, const cVector3f &avEnd, cColor aCol);
	void DrawBoxMaxMin(const cVector3f &avMax, const cVector3f &avMin, cColor aCol);
	void DrawSphere(const cVector3f &avPos, float afRadius, cColor aCol);

	void DrawLine2D(const cVector2f &avBegin, const cVector2f &avEnd, float afZ, cColor aCol);
	void DrawLineRect2D(const cRect2f &aRect, float afZ, cColor aCol);
	void DrawLineCircle2D(const cVector2f &avCenter, float afRadius, float afZ, cColor aCol);

	void DrawFilledRect2D(const cRect2f &aRect, float afZ, cColor aCol);

	// FRAMEBUFFER
	void CopyContextToTexure(iTexture *apTex, const cVector2l &avPos,
							 const cVector2l &avSize, const cVector2l &avTexOffset = 0);
	void SetRenderTarget(iTexture *pTex);
	bool RenderTargetHasZBuffer();
	void FlushRenderTarget();

	void FlushRendering();
	void SwapBuffers();

	///// SDL Specific ////////////////////////////

	void SetupGL();

    void drawVertices(const MetalShaderVertex *vertices, uint count, const uint32 *indices,
                      uint indexCount, eVertexBufferDrawType type, const Graphics::Metal::GeometryBinding *geometry = nullptr);
    void bindProgram(MetalGpuProgram *program) { _program = program; }
    void releaseProgram(MetalGpuProgram *program) { if (_program == program) _program = nullptr; }
    void releaseTexture(iTexture *texture);
    iTexture *currentRenderTarget() const { return _renderTarget; }
    cMatrixf matrix(eMatrix type) const;
    Graphics::Metal::Renderer &metal() const { return *_metal; }
    const MetalShaderUniforms &shaderState() const { return _uniforms; }

private:
    Graphics::Metal::Renderer *_metal = nullptr;
    Common::HashMap<uint32, Graphics::Metal::Program *> _fixedPrograms;
    Graphics::Metal::Program *_gammaProgram = nullptr;
    Graphics::Metal::Texture *_gammaTexture = nullptr;
    MetalGpuProgram *_program = nullptr;
    Graphics::Metal::DrawState _state;
    uint _alphaFunction = Graphics::Metal::kAlways;
    bool _alphaActive = false;
    MetalShaderUniforms _uniforms = {};
    Common::Array<cMatrixf> _matrixStacks[10];
    cMatrixf _matrices[10];
    iTexture *_textures[8] = {};
    Graphics::Metal::Texture *_white = nullptr, *_whiteCube = nullptr, *_whiteVolume = nullptr;
    Common::Array<MetalShaderVertex> _batch;
    Common::Array<uint32> _batchIndices;
    Common::Array<cVector3f> _batchTexcoords[8];
    bool _batchUnitActive[8] = {};
    cColor _clearColor = cColor(0, 0, 0, 1), _currentColor = cColor(1, 1, 1, 1);
    bool _clearColorActive = true, _clearDepthActive = true, _clearStencilActive = true;
    float _clearDepth = 1, _gamma = 1;
    int _clearStencil = 0, _multisampling = 0, _activeUnit = 0;
    Graphics::PixelFormat _pixelFormat = Graphics::PixelFormat::createFormatRGBA32();
    cVector2f _screenSize, _virtualSize;
    cPlanef _clipPlanes[6];
    cVector3f _normal = cVector3f(0, 0, 1);
    iTexture *_renderTarget = nullptr;
    void flushBatch(eVertexBufferDrawType type, tVtxBatchFlag flags, bool clear);
};

} // End of namespace hpl

#endif
