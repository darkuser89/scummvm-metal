/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HPL_METAL_RESOURCES_H
#define HPL_METAL_RESOURCES_H

#include "hpl1/engine/graphics/GPUProgram.h"
#include "hpl1/engine/graphics/Texture.h"
#include "hpl1/engine/impl/metal_shader_types.h"
#include "graphics/metal/renderer.h"

namespace hpl {

class LowLevelGraphicsMetal;

class MetalGpuProgram : public iGpuProgram {
public:
	MetalGpuProgram(LowLevelGraphicsMetal *graphics, const tString &vertex, const tString &fragment);
	~MetalGpuProgram() override;
	bool reload() override;
	void unload() override;
	void destroy() override { unload(); }
	void Bind() override;
	void UnBind() override;
	bool SetFloat(const tString &name, float x) override;
	bool SetVec2f(const tString &name, float x, float y) override;
	bool SetVec3f(const tString &name, float x, float y, float z) override;
	bool SetVec4f(const tString &name, float x, float y, float z, float w) override;
	bool SetMatrixf(const tString &name, const cMatrixf &matrix) override;
	bool SetMatrixf(const tString &name, eGpuProgramMatrix type, eGpuProgramMatrixOp operation) override;
	Graphics::Metal::Program *program() const { return _program; }
	MetalShaderUniforms &uniforms() { return _uniforms; }
	uint textureKind(uint index) const { return _textureKinds[index]; }

private:
	LowLevelGraphicsMetal *_graphics;
	Graphics::Metal::Program *_program = nullptr;
	tString _vertex, _fragment;
	MetalShaderUniforms _uniforms = {};
	uint8 _textureKinds[8] = {};
	bool set(const tString &name, const float *values, uint count);
};

class MetalTexture : public iTexture {
public:
	MetalTexture(const tString &name, Graphics::PixelFormat *format, iLowLevelGraphics *graphics,
	             eTextureType type, bool mipmaps, eTextureTarget target);
	~MetalTexture() override;
	bool CreateFromBitmap(Bitmap2D *bitmap) override;
	bool CreateCubeFromBitmapVec(tBitmap2DVec *bitmaps) override;
	bool Create(uint width, uint height, cColor color) override;
	bool CreateAnimFromBitmapVec(tBitmap2DVec *bitmaps) override;
	bool CreateFromArray(unsigned char *pixels, int channels, const cVector3l &size) override;
	void Update(float step) override;
	void SetFilter(eTextureFilter filter) override { mFilter = filter; }
	void SetAnisotropyDegree(float value) override { mfAnisotropyDegree = value; }
	float GetGamma() override { return 0; }
	void SetGamma(float gamma) override {}
	int GetHandle() override { return _id; }
	void SetWrapS(eTextureWrap mode) override { mWrapS = mode; }
	void SetWrapT(eTextureWrap mode) override { mWrapT = mode; }
	void SetWrapR(eTextureWrap mode) override { mWrapR = mode; }
	bool HasAnimation() override { return _frames.size() > 1; }
	void NextFrame() override;
	void PrevFrame() override;
	float GetT() override;
	float GetTimeCount() override { return _time; }
	void SetTimeCount(float time) override { _time = time; }
	int GetCurrentLowlevelHandle() override;
	bool hasAlpha() override { return _alpha; }
	Graphics::Metal::Texture *texture() const;
	Graphics::Metal::RenderTarget *target() const { return _target; }

private:
	LowLevelGraphicsMetal *_graphics;
	Common::Array<Graphics::Metal::Texture *> _frames;
	Graphics::Metal::RenderTarget *_target = nullptr;
	int _id;
	float _time = 0, _direction = 1;
	bool _alpha = false;
	void clear();
	bool appendBitmap(Bitmap2D *bitmap);
	void advance(float amount);
};

} // End of namespace hpl

#endif
