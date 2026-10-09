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

/*
 * This file is based on WME.
 * http://dead-code.org/redir.php?target=wme
 * Copyright (c) 2003-2013 Jan Nedoma and contributors
 */

#include "engines/wintermute/base/base_game.h"
#include "engines/wintermute/dcgf.h"


#if defined(USE_METAL)

#include "engines/wintermute/base/gfx/metal/base_render_metal3d.h"
#include "engines/wintermute/base/gfx/metal/shadow_volume_metal.h"

namespace Wintermute {

using namespace MetalBackend;

struct ShadowVertexShader {
	float x;
	float y;
	float z;
};

//////////////////////////////////////////////////////////////////////////
ShadowVolumeMetal::ShadowVolumeMetal(BaseGame *inGame, MetalBackend::Shader *volumeShader, MetalBackend::Shader *maskShader)
	: ShadowVolume(inGame), _volumeShader(volumeShader), _maskShader(maskShader) {
	_shadowVolumeVertexBuffer = 0;

	metalGenBuffers(1, &_shadowVolumeVertexBuffer);
	metalBindBuffer(kArrayBuffer, _shadowVolumeVertexBuffer);
	metalBufferData(kArrayBuffer, 12 * _vertices.getSize(), _vertices.getData(), kStaticDraw);
	metalBindBuffer(kArrayBuffer, 0);

	metalGenBuffers(1, &_shadowMaskVertexBuffer);
	metalBindBuffer(kArrayBuffer, _shadowMaskVertexBuffer);
	metalBufferData(kArrayBuffer, 4 * sizeof(ShadowVertexShader), nullptr, kStaticDraw);
	metalBindBuffer(kArrayBuffer, 0);
}

//////////////////////////////////////////////////////////////////////////
ShadowVolumeMetal::~ShadowVolumeMetal() {
	metalDeleteBuffers(1, &_shadowVolumeVertexBuffer);
	metalDeleteBuffers(1, &_shadowMaskVertexBuffer);
}

//////////////////////////////////////////////////////////////////////////
bool ShadowVolumeMetal::render() {
	metalBindTexture(kTexture2D, 0);
	metalDisable(kTexture2D);
	_game->_renderer3D->_lastTexture = nullptr;

	metalDrawArrays(kTriangles, 0, _vertices.getSize());

	return true;
}

//////////////////////////////////////////////////////////////////////////
bool ShadowVolumeMetal::renderToStencilBuffer() {
	// since the vertex count of the volume might change,
	// we just create a new buffer per frame
	// we might as well use the number of vertices of the mesh as an upper bound
	// or get rid of this completely by moving everything onto the gpu
	metalDeleteBuffers(1, &_shadowVolumeVertexBuffer);
	metalGenBuffers(1, &_shadowVolumeVertexBuffer);
	metalBindBuffer(kArrayBuffer, _shadowVolumeVertexBuffer);
	metalBufferData(kArrayBuffer, 12 * _vertices.getSize(), _vertices.getData(), kStaticDraw);

	_volumeShader->enableVertexAttribute("position", _shadowVolumeVertexBuffer, 3, kFloat, false, 12, 0);
	_volumeShader->use(true);

	// Disable z-buffer/color writes (note: z-testing still occurs), and enable the
	// stencil-buffer
	metalDepthMask(kFalse);
	metalColorMask(kFalse, kFalse, kFalse, kFalse);
	metalDisable(kTexture2D);
	metalEnable(kStencilTest);
	metalEnable(kCullFace);

	// Set up stencil compare fuction, reference value, and masks.
	// Stencil test passes if ((ref & mask) cmpfn (stencil & mask)) is true.
	// Note: since we set up the stencil-test to always pass, the STENCILFAIL
	// renderstate is really not needed.
	metalStencilFunc(kAlways, 0x1, (uint32)~0);

	metalStencilOp(kKeep, kKeep, kIncr);

	// Draw back-side of shadow volume in stencil/z only
	metalFrontFace(kCcw);
	render();

	// Decrement stencil buffer value
	metalStencilOp(kKeep, kKeep, kDecr);

	// Draw front-side of shadow volume in stencil/z only
	metalFrontFace(kCw);
	render();

	// Restore render states
	metalFrontFace(kCcw);
	metalDepthMask(kTrue);
	metalColorMask(kTrue, kTrue, kTrue, kTrue);
	metalDisable(kStencilTest);
	metalDisable(kBlend);

	return true;
}

//////////////////////////////////////////////////////////////////////////
bool ShadowVolumeMetal::renderToScene() {
	initMask();

	metalDisable(kDepthTest);
	metalEnable(kStencilTest);
	metalEnable(kBlend);
	metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);

	// Only write where stencil val >= 1 (count indicates # of shadows that overlap that pixel)
	metalStencilFunc(kLequal, 0x1, (uint32)~0);
	metalStencilOp(kKeep, kKeep, kKeep);

	metalBindTexture(kTexture2D, 0);

	BaseRenderMetal3D *renderer = dynamic_cast<BaseRenderMetal3D *>(_game->_renderer3D);
	renderer->_shadowMaskShader->use();
	renderer->setProjection2D(renderer->_shadowMaskShader);

	_maskShader->enableVertexAttribute("position", _shadowMaskVertexBuffer, 3, kFloat, false, 12, 0);
	_maskShader->use(true);

	metalFrontFace(kCw);

	metalDrawArrays(kTriangleStrip, 0, 4);

	// Restore render states
	metalEnable(kDepthTest);
	metalDisable(kStencilTest);

	_game->_renderer3D->setup3D(nullptr, true);

	// clear stencil buffer
	metalClearStencil(0);
	metalClear(kStencilBufferBit);

	return true;
}

//////////////////////////////////////////////////////////////////////////
bool ShadowVolumeMetal::initMask() {
	auto *rend = _game->_renderer3D;
	ShadowVertexShader shadowMask[4];

	// bottom left
	shadowMask[0].x = 0.0f;
	shadowMask[0].y = rend->getHeight();
	shadowMask[0].z = 1.0f;

	// top left
	shadowMask[1].x = 0.0f;
	shadowMask[1].y = 0.0f;
	shadowMask[1].z = 1.0f;

	// bottom right
	shadowMask[2].x = rend->getWidth();
	shadowMask[2].y = rend->getHeight();
	shadowMask[2].z = 1.0f;

	// top right
	shadowMask[3].x = rend->getWidth();
	shadowMask[3].y = 0.0f;
	shadowMask[3].z = 1.0f;

	metalBindBuffer(kArrayBuffer, _shadowMaskVertexBuffer);
	metalBufferSubData(kArrayBuffer, 0, 4 * sizeof(ShadowVertexShader), shadowMask);

	Math::Vector4d color;

	color.x() = RGBCOLGetR(_color) / 255.0f;
	color.y() = RGBCOLGetG(_color) / 255.0f;
	color.z() = RGBCOLGetB(_color) / 255.0f;
	color.w() = RGBCOLGetA(_color) / 255.0f;

	_maskShader->use();
	_maskShader->setUniform("color", color);

	return true;
}

} // namespace Wintermute

#endif // defined(USE_METAL)
