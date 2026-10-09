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

#include "engines/wintermute/base/gfx/xmaterial.h"
#include "engines/wintermute/base/gfx/3deffect.h"
#include "engines/wintermute/base/gfx/3deffect_params.h"
#include "engines/wintermute/base/gfx/skin_mesh_helper.h"
#include "engines/wintermute/base/gfx/base_renderer3d.h"
#include "engines/wintermute/base/base_game.h"
#include "engines/wintermute/base/base_engine.h"


#if defined(USE_METAL)

#include "engines/wintermute/base/gfx/metal/base_surface_metal3d.h"
#include "engines/wintermute/base/gfx/metal/base_render_metal3d.h"
#include "engines/wintermute/base/gfx/metal/meshx_metal.h"

namespace Wintermute {

using namespace MetalBackend;

//////////////////////////////////////////////////////////////////////////
XMeshMetal::XMeshMetal(BaseGame *inGame, MetalBackend::Shader *shader, MetalBackend::Shader *flatShadowShader) :
	XMesh(inGame), _shader(shader), _flatShadowShader(flatShadowShader) {
	metalGenBuffers(1, &_vertexBuffer);
	metalGenBuffers(1, &_indexBuffer);
}

//////////////////////////////////////////////////////////////////////////
XMeshMetal::~XMeshMetal() {
	metalDeleteBuffers(1, &_vertexBuffer);
	metalDeleteBuffers(1, &_indexBuffer);
}

bool XMeshMetal::loadFromXData(const char *filename, XFileData *xobj) {
	if (XMesh::loadFromXData(filename, xobj)) {
		uint32 *indexData = (uint32 *)_blendedMesh->getIndexBuffer().ptr();
		uint32 indexDataSize = _blendedMesh->getIndexBuffer().size() / sizeof(uint32);
		float *vertexData = (float *)_blendedMesh->getVertexBuffer().ptr();
		uint32 vertexSize = DXGetFVFVertexSize(_blendedMesh->getFVF()) / sizeof(float);
		uint32 vertexCount = _blendedMesh->getNumVertices();

		metalBindBuffer(kArrayBuffer, _vertexBuffer);
		metalBufferData(kArrayBuffer, 4 * vertexSize * vertexCount, vertexData, kDynamicDraw);

		metalBindBuffer(kElementArrayBuffer, _indexBuffer);
		metalBufferData(kElementArrayBuffer, 4 * indexDataSize, indexData, kStaticDraw);

		return true;
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////
bool XMeshMetal::render(XModel *model) {
	if (!_blendedMesh)
		return false;

	// For WME DX, mesh model is not visible, possible it's clipped.
	// For OpenGL, mesh is visible, skip draw it here instead in core.
	if (!_game->_renderer3D->_camera)
		return false;

	auto fvf = _blendedMesh->getFVF();
	uint32 vertexSize = DXGetFVFVertexSize(fvf) / sizeof(float);
	float *vertexData = (float *)_blendedMesh->getVertexBuffer().ptr();
	if (vertexData == nullptr) {
		return false;
	}
	uint32 offset = 0, normalOffset = 0, textureOffset = 0;
	if (fvf & DXFVF_XYZ) {
		offset += sizeof(DXVector3) / sizeof(float);
	}
	if (fvf & DXFVF_NORMAL) {
		normalOffset = offset;
		offset += sizeof(DXVector3) / sizeof(float);
	}
	if (fvf & DXFVF_DIFFUSE) {
		offset += sizeof(DXColorValue) / sizeof(float);
	}
	if (fvf & DXFVF_TEX1) {
		textureOffset = offset;
	}

	bool noAttrs = false;
	auto attrsTable = _blendedMesh->getAttributeTable();
	uint32 numAttrs = attrsTable->_size;
	DXAttributeRange *attrs;
	if (numAttrs == 0) {
		noAttrs = true;
		numAttrs = 1;
		attrs = new DXAttributeRange[numAttrs];
	} else {
		attrs = attrsTable->_ptr;
	}

	if (noAttrs) {
		attrs[0]._attribId = 0;
		attrs[0]._vertexStart = attrs[0]._faceStart = 0;
		attrs[0]._vertexCount = _blendedMesh->getNumVertices();
		attrs[0]._faceCount = _blendedMesh->getNumFaces();
	}

	metalBindBuffer(kElementArrayBuffer, _indexBuffer);

	_shader->enableVertexAttribute("position", _vertexBuffer, 3, kFloat, false, 4 * vertexSize, 0);
	_shader->enableVertexAttribute("texcoord", _vertexBuffer, 2, kFloat, false, 4 * vertexSize, 4 * textureOffset);
	_shader->enableVertexAttribute("normal", _vertexBuffer, 3, kFloat, false, 4 * vertexSize, 4 * normalOffset);
	_shader->use(true);

	for (uint32 i = 0; i < numAttrs; i++) {
		Material *mat = _materials[attrs[i]._attribId];
		if (mat->getSurface()) {
			metalEnable(kTexture2D);
			static_cast<BaseSurfaceMetal3D *>(mat->getSurface())->setTexture();
			metalTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
			metalTexParameteri(kTexture2D, kTextureMinFilter, kLinearMipmapLinear);
			metalGenerateMipmap(kTexture2D);
			_shader->setUniform("useTexture", true);
		} else {
			_shader->setUniform("useTexture", false);
			metalBindTexture(kTexture2D, 0);
			metalDisable(kTexture2D);
		}

		Math::Vector4d diffuse(mat->_material._diffuse._data);
		_shader->setUniform("diffuse", diffuse);
		_shader->setUniform("ambient", diffuse);
		_shader->setUniform("effectId", 0);

		Math::Vector4d shadowLightPos;
		shadowLightPos.x() = model->_shadowLightPos._x;
		shadowLightPos.y() = model->_shadowLightPos._y;
		shadowLightPos.z() = model->_shadowLightPos._z;
		shadowLightPos.w() = 0.0f;
		_shader->setUniform("shadowLightPos", shadowLightPos);
		setupEffect(mat, false);
		size_t offsetFace = 4 * attrs[i]._faceStart * 3;
		metalDrawElements(kTriangles, attrs[i]._faceCount * 3, kUnsignedInt, (void *)offsetFace);
		if (setupEffect(mat, true)) {
			bool stateCullFace;
			metalGetBooleanv(kCullFace, &stateCullFace);
			metalDrawElements(kTriangles, attrs[i]._faceCount * 3, kUnsignedInt, (void *)offsetFace);
			if (stateCullFace)
				metalEnable(kCullFace);
			else
				metalDisable(kCullFace);
		}
	}

	metalBindTexture(kTexture2D, 0);
	metalDisable(kTexture2D);

	metalBindBuffer(kArrayBuffer, 0);
	metalBindBuffer(kElementArrayBuffer, 0);

	if (noAttrs) {
		delete[] attrs;
	}

	return true;
}

bool XMeshMetal::renderFlatShadowModel(uint32 shadowColor) {
	if (!_blendedMesh)
		return false;

	// For WME DX, mesh model is not visible, possible it's clipped.
	// For OpenGL, mesh is visible, skip draw it here instead in core.
	if (!_game->_renderer3D->_camera)
		return false;

	// W/A for the scene with the table in the laboratory where the engine switches to flat shadows.
	// Presumably, it's supposed to disable shadows.
	// Instead, OpenGL draws graphical glitches.
	// Original DX version does not have this issue due to rendering shadows differently.
	if (BaseEngine::instance().getGameId() == "alphapolaris")
		return false;
	
	uint32 vertexSize = DXGetFVFVertexSize(_blendedMesh->getFVF()) / sizeof(float);
	float *vertexData = (float *)_blendedMesh->getVertexBuffer().ptr();
	if (vertexData == nullptr) {
		return false;
	}

	bool noAttrs = false;
	auto attrsTable = _blendedMesh->getAttributeTable();
	uint32 numAttrs = attrsTable->_size;
	DXAttributeRange *attrs;
	if (numAttrs == 0) {
		noAttrs = true;
		numAttrs = 1;
		attrs = new DXAttributeRange[numAttrs];
	} else {
		attrs = attrsTable->_ptr;
	}

	if (noAttrs) {
		attrs[0]._attribId = 0;
		attrs[0]._vertexStart = attrs[0]._faceStart = 0;
		attrs[0]._vertexCount = _blendedMesh->getNumVertices();
		attrs[0]._faceCount = _blendedMesh->getNumFaces();
	}

	Math::Vector4d color;
	color.x() = RGBCOLGetR(shadowColor) / 255.0f;
	color.y() = RGBCOLGetG(shadowColor) / 255.0f;
	color.z() = RGBCOLGetB(shadowColor) / 255.0f;
	color.w() = RGBCOLGetA(shadowColor) / 255.0f;

	_flatShadowShader->enableVertexAttribute("position", _vertexBuffer, 3, kFloat, false, 4 * vertexSize, 0);
	_flatShadowShader->use(true);

	metalColorMask(kFalse, kFalse, kFalse, kFalse);
	metalDepthMask(kFalse);

	metalEnable(kStencilTest);
	metalStencilFunc(kAlways, 1, (uint32)~0);
	metalStencilOp(kReplace, kReplace, kReplace);

	metalBindBuffer(kElementArrayBuffer, _indexBuffer);

	for (uint32 i = 0; i < numAttrs; i++) {
		size_t offsetFace = 4 * attrs[i]._faceStart * 3;
		metalDrawElements(kTriangles, attrs[i]._faceCount * 3, kUnsignedInt, (void *)offsetFace);
	}

	metalStencilFunc(kEqual, 1, (uint32)~0);
	metalStencilOp(kZero, kZero, kZero);

	_flatShadowShader->setUniform("shadowColor", color);
	metalColorMask(kTrue, kTrue, kTrue, kTrue);
	metalEnable(kBlend);
	metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);

	metalDepthMask(kTrue);

	for (uint32 i = 0; i < numAttrs; i++) {
		size_t offsetFace = 4 * attrs[i]._faceStart * 3;
		metalDrawElements(kTriangles, attrs[i]._faceCount * 3, kUnsignedInt, (void *)offsetFace);
	}

	metalBindBuffer(kArrayBuffer, 0);
	metalBindBuffer(kElementArrayBuffer, 0);

	if (noAttrs) {
		delete[] attrs;
	}

	metalDisable(kBlend);
	metalDisable(kStencilTest);

	return true;
}

bool XMeshMetal::update(FrameNode *parentFrame) {
	XMesh::update(parentFrame);

	float *vertexData = (float *)_blendedMesh->getVertexBuffer().ptr();
	uint32 vertexSize = DXGetFVFVertexSize(_blendedMesh->getFVF()) / sizeof(float);
	uint32 vertexCount = _blendedMesh->getNumVertices();

	metalBindBuffer(kArrayBuffer, _vertexBuffer);
	metalBufferSubData(kArrayBuffer, 0, 4 * vertexSize * vertexCount, vertexData);

	return true;
}

bool XMeshMetal::setupEffect(Material *material, bool secondPassNeeded) {
	auto effect = material->getEffect();
	if (!effect)
		return false;
	auto effectId = effect->getEffectHash();
	switch (effectId) {
	case 0x589E1D5C: // "actors\\abbey\\MyBorderGlow.fx"
	case 0x699AF321: // "actors\\watcher\\MyBorderGlow.fx"
	case 0x1255105D: // "actors\\Jackie\\MyBorderGlow.fx"
	{
		_shader->setUniform("effectId", 1);
		auto params = material->getEffectParams();
		auto param = params->getParamByName("SceneAmbientLight");
		DXVector4 ambientColor;
		param->getValue(ambientColor);
		Math::Vector4d ambient(ambientColor);
		float borderWidth = 0.0f;
		if (effectId == 0x589E1D5C) {
			borderWidth = 6.0f;
			ambient = { 1.0, 1.0, 1.0, 1.0 };
		}
		if (effectId == 0x699AF321)
			borderWidth = 10.0f;
		if (effectId == 0x1255105D)
			borderWidth = 8.0f;
		_shader->setUniform("ambient", ambient);
		_shader->setUniform1f("borderWidth", borderWidth);
		_shader->setUniform("effectSecondPass", secondPassNeeded);
		if (secondPassNeeded) {
			metalCullFace(kBack);
			metalDisable(kCullFace);
			metalDepthMask(kTrue);
		} else {
			metalCullFace(kFront);
			metalEnable(kCullFace);
			metalDepthMask(kFalse);
		}
		return true;
	}
	case 0x2AD31E3B: // "effects\\outer_glow.fx"
	{
		_shader->setUniform("effectId", 2);
		return false;
	}
	default:
		warning("Unknown effect hash: %x", effectId);
		return false;
	}
}

} // namespace Wintermute

#endif // defined(USE_METAL)
