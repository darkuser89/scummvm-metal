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

// Matrix calculations taken from the glm library
// Which is covered by the MIT license
// And has this additional copyright note:
/* Copyright (c) 2005 - 2012 G-Truc Creation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 */

#include "common/endian.h"
#include "common/file.h"
#include "common/str.h"
#include "common/system.h"
#include "common/textconsole.h"

#if defined(USE_METAL)

#include "graphics/cursorman.h"
#include "graphics/surface.h"
#include "engines/grim/metal_draw.h"

#include "engines/grim/actor.h"
#include "engines/grim/bitmap.h"
#include "engines/grim/colormap.h"
#include "engines/grim/emi/modelemi.h"
#include "engines/grim/font.h"
#include "engines/grim/gfx_metal.h"
#include "engines/grim/grim.h"
#include "engines/grim/material.h"
#include "engines/grim/model.h"
#include "engines/grim/primitives.h"
#include "engines/grim/set.h"
#include "engines/grim/sprite.h"

namespace Grim {

using namespace MetalBackend;

template<class T>
static T nextHigher2(T k) {
	if (k == 0)
		return 1;
	--k;

	for (uint i = 1; i < sizeof(T) * 8; i <<= 1)
		k = k | k >> i;

	return k + 1;
}

static float textured_quad[] = {
//	X   , Y   , S   , T
	0.0f, 0.0f, 0.0f, 0.0f,
	1.0f, 0.0f, 1.0f, 0.0f,
	1.0f, 1.0f, 1.0f, 1.0f,
	0.0f, 1.0f, 0.0f, 1.0f,
};

static float zero_texVerts[] = { 0.0, 0.0 };

struct GrimVertex {
	GrimVertex(const float *verts, const float *texVerts, const float *normals) {
		memcpy(_position, verts, 3 * sizeof(float));
		memcpy(_texcoord, texVerts, 2 * sizeof(float));
		memcpy(_normal, normals, 3 * sizeof(float));
	}
	float _position[3];
	float _texcoord[2];
	float _normal[3];
};

struct TextUserData {
	MetalBackend::Shader * shader = nullptr;
	uint32 characters = 0;
	Color  color;
	uint32 texture = 0;
	bool ownsVertexBuffer = false;
	Common::Array<uint32> lineTextures;
	Common::Array<Math::Vector2d> lineSizes;
};

struct FontUserData {
	int size;
	uint32 texture;
};

struct EMIModelUserData {
	MetalBackend::Shader *_shader;
	MetalBackend::Shader *_shaderLights;
	uint32 _texCoordsVBO;
	uint32 _colorMapVBO;
	uint32 _verticesVBO;
	uint32 _normalsVBO;
};

struct ModelUserData {
	MetalBackend::Shader *_shader;
	MetalBackend::Shader *_shaderLights;
	uint32 _meshInfoVBO;
};

struct ShadowUserData {
	uint32 _verticesVBO;
	uint32 _indicesVBO;
	uint32 _numTriangles;
};

Math::Matrix4 metalMakeLookMatrix(const Math::Vector3d& pos, const Math::Vector3d& interest, const Math::Vector3d& up) {
	Math::Vector3d f = (interest - pos).getNormalized();
	Math::Vector3d u = up.getNormalized();
	Math::Vector3d s = Math::Vector3d::crossProduct(f, u).getNormalized();
	u = Math::Vector3d::crossProduct(s, f);

	Math::Matrix4 look;
	look(0, 0) = s.x();
	look(1, 0) = s.y();
	look(2, 0) = s.z();
	look(0, 1) = u.x();
	look(1, 1) = u.y();
	look(2, 1) = u.z();
	look(0, 2) = -f.x();
	look(1, 2) = -f.y();
	look(2, 2) = -f.z();
	look(3, 0) = -Math::Vector3d::dotProduct(s, pos);
	look(3, 1) = -Math::Vector3d::dotProduct(u, pos);
	look(3, 2) =  Math::Vector3d::dotProduct(f, pos);

	look.transpose();

	return look;
}

Math::Matrix4 metalMakeRotationMatrix(const Math::Angle& angle, Math::Vector3d axis) {
	float c = angle.getCosine();
	float s = angle.getSine();
	axis.normalize();
	Math::Vector3d temp = (1.f - c) * axis;
	Math::Matrix4 rotate;
	rotate(0, 0) = c + temp.x() * axis.x();
	rotate(0, 1) = 0 + temp.x() * axis.y() + s * axis.z();
	rotate(0, 2) = 0 + temp.x() * axis.z() - s * axis.y();
	rotate(0, 3) = 0;
	rotate(1, 0) = 0 + temp.y() * axis.x() - s * axis.z();
	rotate(1, 1) = c + temp.y() * axis.y();
	rotate(1, 2) = 0 + temp.y() * axis.z() + s * axis.x();
	rotate(1, 3) = 0;
	rotate(2, 0) = 0 + temp.z() * axis.x() + s * axis.y();
	rotate(2, 1) = 0 + temp.z() * axis.y() - s * axis.x();
	rotate(2, 2) = c + temp.z() * axis.z();
	rotate(2, 3) = 0;
	rotate(3, 0) = 0;
	rotate(3, 1) = 0;
	rotate(3, 2) = 0;
	rotate(3, 3) = 1;

	return rotate;
}

Math::Matrix4 metalMakeFrustumMatrix(double left, double right, double bottom, double top, double nclip, double fclip) {
	Math::Matrix4 proj;
	proj(0, 0) = (2.0f * nclip) / (right - left);
	proj(1, 1) = (2.0f * nclip) / (top - bottom);
	proj(2, 0) = (right + left) / (right - left);
	proj(2, 1) = (top + bottom) / (top - bottom);
	proj(2, 2) = -(fclip + nclip) / (fclip - nclip);
	proj(2, 3) = -1.0f;
	proj(3, 2) = -(2.0f * fclip * nclip) / (fclip - nclip);
	proj(3, 3) = 0.0f;

	return proj;
}

GfxBase *CreateGfxMetal() {
	return new GfxMetal();
}

GfxMetal::GfxMetal() {
	type = Graphics::RendererType::kRendererTypeMetal;
	_smushTexId = 0;
	_matrixStack.push(Math::Matrix4());
	_fov = -1.0;
	_nclip = -1;
	_fclip = -1;
	_selectedTexture = nullptr;
	_emergTexture = 0;
	_maxLights = 8;
	_lights = new MetalLight[_maxLights];
	_lightsEnabled = false;
	_hasAmbientLight = false;
	_backgroundProgram = nullptr;
	_smushProgram = nullptr;
	_textProgram = nullptr;
	_emergProgram = nullptr;
	_actorProgram = nullptr;
	_actorLightsProgram = nullptr;
	_spriteProgram = nullptr;
	_primitiveProgram = nullptr;
	_irisProgram = nullptr;
	_shadowPlaneProgram = nullptr;
	_dimProgram = nullptr;
	_dimPlaneProgram = nullptr;
	_dimRegionProgram = nullptr;

	float div = 6.0f;
	_overworldProjMatrix = metalMakeFrustumMatrix(-1.f / div, 1.f / div, -0.75f / div, 0.75f / div, 1.0f / div, 3276.8f);
}

GfxMetal::~GfxMetal() {
	releaseMovieFrame();
	for (unsigned int i = 0; i < _numSpecialtyTextures; i++) {
		destroyTexture(&_specialtyTextures[i]);
	}
	delete[] _lights;

	delete _backgroundProgram;
	delete _smushProgram;
	delete _textProgram;
	delete _emergProgram;
	delete _actorProgram;
	delete _actorLightsProgram;
	delete _spriteProgram;
	delete _primitiveProgram;
	delete _irisProgram;
	delete _shadowPlaneProgram;
	delete _dimProgram;
	delete _dimPlaneProgram;
	delete _dimRegionProgram;
	metalDeleteTextures(1, &_storedDisplay);
	metalDeleteTextures(1, &_emergTexture);
}

void GfxMetal::setupZBuffer() {
	_depthBitmapId = -1;
	int format = kLuminanceAlpha;
	MetalBackend::StateToken ztype = kUnsignedByte;
	float width = _gameWidth;
	float height = _gameHeight;

	metalGenTextures(1, (uint32 *)&_zBufTex);
	metalActiveTexture(kTexture1);
	metalBindTexture(kTexture2D, _zBufTex);
	metalTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
	metalTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
	metalTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
	metalTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
	metalTexImage2D(kTexture2D, 0, format, nextHigher2((int)width), nextHigher2((int)height), 0, format, ztype, nullptr);
	metalActiveTexture(kTexture0);

	_zBufTexCrop = Math::Vector2d(width / nextHigher2((int)width), height / nextHigher2((int)height));
}

void GfxMetal::setupQuadEBO() {
	// FIXME: Probably way too big...
	unsigned short quad_indices[6 * 16384];

	unsigned short start = 0;
	for (unsigned short *p = quad_indices; p < &quad_indices[6 * 1000]; p += 6) {
		p[0] = p[3] = start++;
		p[1] = start++;
		p[2] = p[4] = start++;
		p[5] = start++;
	}

	_quadEBO = MetalBackend::Shader::createBuffer(kElementArrayBuffer, sizeof(quad_indices), quad_indices, kStaticDraw);
}

void GfxMetal::setupTexturedQuad() {
	_smushVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, sizeof(textured_quad), textured_quad, kStaticDraw);
	_smushProgram->enableVertexAttribute("position", _smushVBO, 2, kFloat, kFalse, 4 * sizeof(float), 0);
	_smushProgram->enableVertexAttribute("texcoord", _smushVBO, 2, kFloat, kFalse, 4 * sizeof(float), 2 * sizeof(float));

	_emergProgram->enableVertexAttribute("position", _smushVBO, 2, kFloat, kFalse, 4 * sizeof(float), 0);
	_emergProgram->enableVertexAttribute("texcoord", _smushVBO, 2, kFloat, kFalse, 4 * sizeof(float), 2 * sizeof(float));

	if (g_grim->getGameType() == GType_GRIM) {
		_backgroundProgram->enableVertexAttribute("position", _smushVBO, 2, kFloat, kFalse, 4 * sizeof(float), 0);
		_backgroundProgram->enableVertexAttribute("texcoord", _smushVBO, 2, kFloat, kFalse, 4 * sizeof(float), 2 * sizeof(float));
	} else {
		_dimPlaneProgram->enableVertexAttribute("position", _smushVBO, 2, kFloat, kFalse, 4 * sizeof(float), 0);
	}
}

void GfxMetal::setupTexturedCenteredQuad() {
	_spriteVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, 0, nullptr, kStaticDraw);
	_spriteProgram->enableVertexAttribute("position", _spriteVBO, 3, kFloat, kFalse, 5 * sizeof(float), 0);
	_spriteProgram->enableVertexAttribute("texcoord", _spriteVBO, 2, kFloat, kFalse, 5 * sizeof(float), 3 * sizeof(float));
	_spriteProgram->disableVertexAttribute("color", Math::Vector4d(1.0f, 1.0f, 1.0f, 1.0f));
}

void GfxMetal::setupPrimitives() {
	uint32 numVBOs = ARRAYSIZE(_primitiveVBOs);
	metalGenBuffers(numVBOs, _primitiveVBOs);
	_currentPrimitive = 0;
	for (uint32 i = 0; i < numVBOs; ++i) {
		metalBindBuffer(kArrayBuffer, _primitiveVBOs[i]);
		metalBufferData(kArrayBuffer, 8 * sizeof(float), nullptr, kDynamicDraw);
	}

	metalGenBuffers(1, &_irisVBO);
	metalBindBuffer(kArrayBuffer, _irisVBO);
	metalBufferData(kArrayBuffer, 20 * sizeof(float), nullptr, kDynamicDraw);

	_irisProgram->enableVertexAttribute("position", _irisVBO, 2, kFloat, kFalse, 2 * sizeof(float), 0);

	metalGenBuffers(1, &_dimVBO);
	metalBindBuffer(kArrayBuffer, _dimVBO);

	float points[12] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
		1.0f, 1.0f,
		0.0f, 1.0f,
		0.0f, 0.0f,
	};

	metalBufferData(kArrayBuffer, 12 * sizeof(float), points, kDynamicDraw);

	_dimProgram->enableVertexAttribute("position", _dimVBO, 2, kFloat, kFalse, 2 * sizeof(float), 0);
	_dimProgram->enableVertexAttribute("texcoord", _dimVBO, 2, kFloat, kFalse, 2 * sizeof(float), 0);

	metalGenBuffers(1, &_dimRegionVBO);
	metalBindBuffer(kArrayBuffer, _dimRegionVBO);

	metalBufferData(kArrayBuffer, 24 * sizeof(float), nullptr, kDynamicDraw);

	_dimRegionProgram->enableVertexAttribute("position", _dimRegionVBO, 2, kFloat, kFalse, 4 * sizeof(float), 0);
	_dimRegionProgram->enableVertexAttribute("texcoord", _dimRegionVBO, 2, kFloat, kFalse, 4 * sizeof(float), 2 * sizeof(float));

	metalBindBuffer(kArrayBuffer, 0);
}

uint32 GfxMetal::nextPrimitive() {
	uint32 ret = _primitiveVBOs[_currentPrimitive];
	_currentPrimitive = (_currentPrimitive + 1) % ARRAYSIZE(_primitiveVBOs);
	return ret;
}

void GfxMetal::setupShaders() {
	bool isEMI = g_grim->getGameType() == GType_MONKEY4;

	static const char* commonAttributes[] = {"position", "texcoord", nullptr};
	_backgroundProgram = MetalBackend::Shader::fromFiles(isEMI ? "emi_background" : "grim_background", commonAttributes);
	_smushProgram = MetalBackend::Shader::fromFiles("grim_smush", commonAttributes);
	_textProgram = MetalBackend::Shader::fromFiles("grim_text", commonAttributes);
	_emergProgram = MetalBackend::Shader::fromFiles("grim_emerg", commonAttributes);

	static const char* actorAttributes[] = {"position", "texcoord", "color", "normal", nullptr};
	_actorProgram = MetalBackend::Shader::fromFiles(isEMI ? "emi_actor" : "grim_actor", actorAttributes);
	_actorLightsProgram = MetalBackend::Shader::fromFiles(isEMI ? "emi_actorlights" : "grim_actorlights", actorAttributes);
	_spriteProgram = MetalBackend::Shader::fromFiles(isEMI ? "emi_sprite" : "grim_actor", actorAttributes);

	static const char* primAttributes[] = { "position", nullptr };
	_shadowPlaneProgram = MetalBackend::Shader::fromFiles("grim_shadowplane", primAttributes);
	_primitiveProgram = MetalBackend::Shader::fromFiles("grim_primitive", primAttributes);

	_irisProgram = _primitiveProgram->clone();
	_dimProgram = MetalBackend::Shader::fromFiles("grim_dim", commonAttributes);
	_dimRegionProgram = _dimProgram->clone();
	_dimPlaneProgram = MetalBackend::Shader::fromFiles("emi_dimplane", primAttributes);

	setupQuadEBO();
	setupTexturedQuad();
	setupTexturedCenteredQuad();
	setupPrimitives();

	_blastVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, 128 * 16 * sizeof(float), nullptr, kDynamicDraw);
}

void GfxMetal::setupScreen(int screenW, int screenH) {
	attach(*g_system->getMetalRenderer());
	_screenWidth = screenW;
	_screenHeight = screenH;
	_scaleW = _screenWidth / (float)_gameWidth;
	_scaleH = _screenHeight / (float)_gameHeight;
	_globalScaleW = _screenWidth / (float)_globalWidth;
	_globalScaleH = _screenHeight / (float)_globalHeight;

	CursorMan.showMouse(false);

	setupZBuffer();
	setupShaders();

	metalViewport(0, 0, _screenWidth, _screenHeight);

	metalGenTextures(1, &_storedDisplay);

	metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
	if (g_grim->getGameType() == GType_MONKEY4) {
		// kLequal as glDepthFunc ensures that subsequent drawing attempts for
		// the same triangles are not ignored by the depth test.
		// That's necessary for EMI where some models have multiple faces which
		// refer to the same vertices. The first face is usually using the
		// color map and the following are using textures.
		metalDepthFunc(kLequal);
	}
}

void GfxMetal::setupCameraFrustum(float fov, float nclip, float fclip) {
	if (_fov == fov && _nclip == nclip && _fclip == fclip)
		return;

	_fov = fov; _nclip = nclip; _fclip = fclip;

	float right = nclip * tan(fov / 2 * ((float)M_PI / 180));
	float top = right * 0.75;

	_projMatrix = metalMakeFrustumMatrix(-right, right, -top, top, nclip, fclip);
}

void GfxMetal::positionCamera(const Math::Vector3d &pos, const Math::Vector3d &interest, float roll) {
	Math::Matrix4 viewMatrix = metalMakeRotationMatrix(Math::Angle(roll), Math::Vector3d(0, 0, 1));
	Math::Vector3d up_vec(0, 0, 1);

	if (pos.x() == interest.x() && pos.y() == interest.y())
		up_vec = Math::Vector3d(0, 1, 0);

	Math::Matrix4 lookMatrix = metalMakeLookMatrix(pos, interest, up_vec);

	_viewMatrix = viewMatrix * lookMatrix;
	_viewMatrix.transpose();
}

void GfxMetal::positionCamera(const Math::Vector3d &pos, const Math::Matrix4 &rot) {
	Math::Matrix4 projMatrix = _projMatrix;
	projMatrix.transpose();

	_currentPos = pos;
	_currentRot = rot;

	Math::Matrix4 invertZ;
	invertZ(2, 2) = -1.0f;

	Math::Matrix4 viewMatrix = _currentRot;
	viewMatrix.transpose();

	Math::Matrix4 camPos;
	camPos(0, 3) = -_currentPos.x();
	camPos(1, 3) = -_currentPos.y();
	camPos(2, 3) = -_currentPos.z();

	_viewMatrix = invertZ * viewMatrix * camPos;
	_mvpMatrix = projMatrix * _viewMatrix;
	_viewMatrix.transpose();
}


Math::Matrix4 GfxMetal::getModelView() {
	if (g_grim->getGameType() == GType_MONKEY4) {
		Math::Matrix4 invertZ;
		invertZ(2, 2) = -1.0f;

		Math::Matrix4 viewMatrix = _currentRot;
		viewMatrix.transpose();

		Math::Matrix4 camPos;
		camPos(0, 3) = -_currentPos.x();
		camPos(1, 3) = -_currentPos.y();
		camPos(2, 3) = -_currentPos.z();

		Math::Matrix4 modelView = invertZ * viewMatrix * camPos;
		return modelView;
	} else {
		return _mvpMatrix;
	}
}

Math::Matrix4 GfxMetal::getProjection() {
	Math::Matrix4 proj = _projMatrix;
	proj.transpose();
	return proj;
}

void GfxMetal::clearScreen() {
	metalClear(kColorBufferBit | kDepthBufferBit);
}

void GfxMetal::clearDepthBuffer() {
	metalClear(kDepthBufferBit);
}

void GfxMetal::flipBuffer(bool opportunistic) {
	// The native renderer retains its resolved color texture between presents.

	g_system->updateScreen();
}

void GfxMetal::getScreenBoundingBox(const Mesh *model, int *x1, int *y1, int *x2, int *y2) {
	if (_currentShadowArray) {
		*x1 = -1;
		*y1 = -1;
		*x2 = -1;
		*y2 = -1;
		return;
	}

	Math::Matrix4 modelMatrix = _currentActor->getFinalMatrix();
	Math::Matrix4 mvpMatrix = _mvpMatrix * modelMatrix;

	double top = 1000;
	double right = -1000;
	double left = 1000;
	double bottom = -1000;

	Math::Vector3d obj;
	float *pVertices = nullptr;

	for (int i = 0; i < model->_numFaces; i++) {
		for (int j = 0; j < model->_faces[i].getNumVertices(); j++) {
			pVertices = model->_vertices + 3 * model->_faces[i].getVertex(j);

			obj.set(*(pVertices), *(pVertices + 1), *(pVertices + 2));

			Math::Vector4d v = Math::Vector4d(obj.x(), obj.y(), obj.z(), 1.0f);
			v = mvpMatrix * v;
			v /= v.w();

			double winX = (1 + v.x()) / 2.0f * _gameWidth;
			double winY = (1 + v.y()) / 2.0f * _gameHeight;

			if (winX > right)
				right = winX;
			if (winX < left)
				left = winX;
			if (winY < top)
				top = winY;
			if (winY > bottom)
				bottom = winY;
		}
	}

	double t = bottom;
	bottom = _gameHeight - top;
	top = _gameHeight - t;

	if (left < 0)
		left = 0;
	if (right >= _gameWidth)
		right = _gameWidth - 1;
	if (top < 0)
		top = 0;
	if (bottom >= _gameHeight)
		bottom = _gameHeight - 1;

	if (top >= _gameHeight || left >= _gameWidth || bottom < 0 || right < 0) {
		*x1 = -1;
		*y1 = -1;
		*x2 = -1;
		*y2 = -1;
		return;
	}

	*x1 = (int)left;
	*y1 = (int)(_gameHeight - bottom);
	*x2 = (int)right;
	*y2 = (int)(_gameHeight - top);
}

void GfxMetal::getScreenBoundingBox(const EMIModel *model, int *x1, int *y1, int *x2, int *y2) {
	if (_currentShadowArray) {
		*x1 = -1;
		*y1 = -1;
		*x2 = -1;
		*y2 = -1;
		return;
	}

	Math::Matrix4 modelMatrix = _currentActor->getFinalMatrix();
	Math::Matrix4 mvpMatrix = _mvpMatrix * modelMatrix;

	double top = 1000;
	double right = -1000;
	double left = 1000;
	double bottom = -1000;

	for (uint i = 0; i < model->_numFaces; i++) {
		uint16 *indices = (uint16 *)model->_faces[i]._indexes;

		for (uint j = 0; j < model->_faces[i]._faceLength * 3; j++) {
			uint16 index = indices[j];
			const Math::Vector3d &dv = model->_drawVertices[index];

			Math::Vector4d v = Math::Vector4d(dv.x(), dv.y(), dv.z(), 1.0f);
			v = mvpMatrix * v;
			v /= v.w();

			double winX = (1 + v.x()) / 2.0f * _gameWidth;
			double winY = (1 + v.y()) / 2.0f * _gameHeight;

			if (winX > right)
				right = winX;
			if (winX < left)
				left = winX;
			if (winY < top)
				top = winY;
			if (winY > bottom)
				bottom = winY;
		}
	}

	double t = bottom;
	bottom = _gameHeight - top;
	top = _gameHeight - t;

	if (left < 0)
		left = 0;
	if (right >= _gameWidth)
		right = _gameWidth - 1;
	if (top < 0)
		top = 0;
	if (bottom >= _gameHeight)
		bottom = _gameHeight - 1;

	if (top >= _gameHeight || left >= _gameWidth || bottom < 0 || right < 0) {
		*x1 = -1;
		*y1 = -1;
		*x2 = -1;
		*y2 = -1;
		return;
	}

	*x1 = (int)left;
	*y1 = (int)(_gameHeight - bottom);
	*x2 = (int)right;
	*y2 = (int)(_gameHeight - top);
}

void GfxMetal::getActorScreenBBox(const Actor *actor, Common::Point &p1, Common::Point &p2) {
	// Get the actor's bounding box information (describes a 3D box)
	Math::Vector3d bboxPos, bboxSize;
	actor->getBBoxInfo(bboxPos, bboxSize);

	// Translate the bounding box to the actor's position
	Math::Matrix4 m = actor->getFinalMatrix();
	bboxPos = bboxPos + actor->getWorldPos();

	// Set up the camera coordinate system
	Math::Matrix4 modelView = _currentRot;
	Math::Matrix4 zScale;
	zScale.setValue(2, 2, -1.0);
	modelView = modelView * zScale;
	modelView.transpose();
	modelView.translate(-_currentPos);
	modelView.transpose();

	// Set values outside of the screen range
	p1.x = 1000;
	p1.y = 1000;
	p2.x = -1000;
	p2.y = -1000;

	// Project all of the points in the 3D bounding box
	Math::Vector3d p, projected;
	for (int x = 0; x < 2; x++) {
		for (int y = 0; y < 2; y++) {
			for (int z = 0; z < 2; z++) {
				Math::Vector3d added(bboxSize.x() * 0.5f * (x * 2 - 1), bboxSize.y() * 0.5f * (y * 2 - 1), bboxSize.z() * 0.5f * (z * 2 - 1));
				m.transform(&added, false);
				p = bboxPos + added;

				Math::Vector4d v = Math::Vector4d(p.x(), p.y(), p.z(), 1.0f);
				v = _projMatrix.transform(modelView.transform(v));
				if (v.w() == 0.0)
					return;
				v /= v.w();

				double winX = (1 + v.x()) / 2.0f * _gameWidth;
				double winY = (1 + v.y()) / 2.0f * _gameHeight;

				// Find the points
				if (winX < p1.x)
					p1.x = winX;
				if (winY < p1.y)
					p1.y = winY;
				if (winX > p2.x)
					p2.x = winX;
				if (winY > p2.y)
					p2.y = winY;
			}
		}
	}

	// Swap the p1/p2 y coorindates
	int16 tmp = p1.y;
	p1.y = 480 - p2.y;
	p2.y = 480 - tmp;
}

void GfxMetal::startActorDraw(const Actor *actor) {
	_currentActor = actor;
	metalEnable(kDepthTest);

	const Math::Vector3d &pos = actor->getWorldPos();
	const Math::Quaternion &quat = actor->getRotationQuat();
	//const float scale = actor->getScale();

	Math::Matrix4 viewMatrix = _viewMatrix;
	viewMatrix.transpose();

	MetalBackend::Shader *shaders[] = { _spriteProgram, _actorProgram, _actorLightsProgram };

	if (g_grim->getGameType() == GType_MONKEY4) {
		metalEnable(kCullFace);
		metalFrontFace(kCw);

		if (actor->isInOverworld())
			viewMatrix = Math::Matrix4();

		Math::Vector4d color(1.0f, 1.0f, 1.0f, actor->getEffectiveAlpha());

		const Math::Matrix4 &viewRot = _currentRot;
		Math::Matrix4 modelMatrix = actor->getFinalMatrix();

		Math::Matrix4 normalMatrix = viewMatrix * modelMatrix;
		normalMatrix.invertAffineOrthonormal();
		modelMatrix.transpose();

		for (int i = 0; i < 3; i++) {
			shaders[i]->use();
			shaders[i]->setUniform("modelMatrix", modelMatrix);
			if (actor->isInOverworld()) {
				shaders[i]->setUniform("viewMatrix", viewMatrix);
				shaders[i]->setUniform("projMatrix", _overworldProjMatrix);
				shaders[i]->setUniform("cameraPos", Math::Vector3d(0,0,0));
			} else {
				shaders[i]->setUniform("viewMatrix", viewRot);
				shaders[i]->setUniform("projMatrix", _projMatrix);
				shaders[i]->setUniform("cameraPos", _currentPos);
			}
			shaders[i]->setUniform("normalMatrix", normalMatrix);

			shaders[i]->setUniform("useVertexAlpha", kFalse);
			shaders[i]->setUniform("uniformColor", color);
			shaders[i]->setUniform1f("alphaRef", 0.0f);
			shaders[i]->setUniform1f("meshAlpha", 1.0f);
		}
	} else {
		Math::Matrix4 modelMatrix = quat.toMatrix();
		bool hasZBuffer = g_grim->getCurrSet()->getCurrSetup()->_bkgndZBm;
		Math::Matrix4 extraMatrix;
		_matrixStack.top() = extraMatrix;

		modelMatrix.transpose();
		modelMatrix.setPosition(pos);
		modelMatrix.transpose();

		for (int i = 0; i < 3; i++) {
			shaders[i]->use();
			shaders[i]->setUniform("modelMatrix", modelMatrix);
			shaders[i]->setUniform("viewMatrix", _viewMatrix);
			shaders[i]->setUniform("projMatrix", _projMatrix);
			shaders[i]->setUniform("extraMatrix", extraMatrix);
			shaders[i]->setUniform("tex", 0);
			shaders[i]->setUniform("texZBuf", 1);
			shaders[i]->setUniform("hasZBuffer", hasZBuffer);
			shaders[i]->setUniform("texcropZBuf", _zBufTexCrop);
			shaders[i]->setUniform("screenSize", Math::Vector2d(_screenWidth, _screenHeight));
			shaders[i]->setUniform1f("alphaRef", 0.5f);
		}
	}

	if (_currentShadowArray) {
		const Sector *shadowSector = _currentShadowArray->planeList.front().sector;
		Math::Vector3d color;
		if (g_grim->getGameType() == GType_GRIM) {
			color = Math::Vector3d(_shadowColorR, _shadowColorG, _shadowColorB) / 255.f;
		} else {
			color = Math::Vector3d(_currentShadowArray->color.getRed(), _currentShadowArray->color.getGreen(), _currentShadowArray->color.getBlue()) / 255.f;
		}
		Math::Vector3d normal = shadowSector->getNormal();
		if (!_currentShadowArray->dontNegate)
			normal = -normal;

		for (int i = 0; i < 3; i++) {
			shaders[i]->use();
			shaders[i]->setUniform("shadow._active", true);
			shaders[i]->setUniform("shadow._color", color);
			shaders[i]->setUniform("shadow._light", _currentShadowArray->pos);
			shaders[i]->setUniform("shadow._point", shadowSector->getVertices()[0]);
			shaders[i]->setUniform("shadow._normal", normal);
		}

		metalDepthMask(kFalse);
		metalDisable(kBlend);
		metalEnable(kPolygonOffsetFill);
	}
	else {
		for (int i = 0; i < 3; i++) {
			shaders[i]->use();
			shaders[i]->setUniform("shadow._active", false);
		}
	}

	_actorLightsProgram->setUniform("hasAmbient", _hasAmbientLight);
	if (_lightsEnabled) {
		// Allocate all variables in one chunk
		static const unsigned int numUniforms = 4;
		static const unsigned int uniformSize = _maxLights * 4;
		float *lightsData = new float[numUniforms * uniformSize];
		for (int i = 0; i < _maxLights; ++i) {
			const MetalLight &l = _lights[i];

			// lightsPosition
			Math::Vector4d tmp = viewMatrix * l._position;

			lightsData[0 * uniformSize + 4 * i + 0] = tmp.x();
			lightsData[0 * uniformSize + 4 * i + 1] = tmp.y();
			lightsData[0 * uniformSize + 4 * i + 2] = tmp.z();
			lightsData[0 * uniformSize + 4 * i + 3] = tmp.w();

			// lightsDirection
			Math::Vector4d direction = l._direction;
			direction.w() = 0.0;
			viewMatrix.transformVector(&direction);
			direction.w() = l._direction.w();

			lightsData[1 * uniformSize + 4 * i + 0] = direction.x();
			lightsData[1 * uniformSize + 4 * i + 1] = direction.y();
			lightsData[1 * uniformSize + 4 * i + 2] = direction.z();
			lightsData[1 * uniformSize + 4 * i + 3] = direction.w();

			// lightsColor
			lightsData[2 * uniformSize + 4 * i + 0] = l._color.x();
			lightsData[2 * uniformSize + 4 * i + 1] = l._color.y();
			lightsData[2 * uniformSize + 4 * i + 2] = l._color.z();
			lightsData[2 * uniformSize + 4 * i + 3] = l._color.w();

			// lightsParams
			lightsData[3 * uniformSize + 4 * i + 0] = l._params.x();
			lightsData[3 * uniformSize + 4 * i + 1] = l._params.y();
			lightsData[3 * uniformSize + 4 * i + 2] = l._params.z();
			lightsData[3 * uniformSize + 4 * i + 3] = l._params.w();
		}

		Common::String uniform;
		int uniformPos;

		uniform = Common::String::format("lightsPosition");
		uniformPos = _actorLightsProgram->getUniformLocation(uniform.c_str());
		if (uniformPos == -1) {
			error("No uniform named '%s'", uniform.c_str());
		}
		metalUniform4fv(uniformPos, _maxLights, &lightsData[0 * uniformSize]);

		uniform = Common::String::format("lightsDirection");
		uniformPos = _actorLightsProgram->getUniformLocation(uniform.c_str());
		if (uniformPos == -1) {
			error("No uniform named '%s'", uniform.c_str());
		}
		metalUniform4fv(uniformPos, _maxLights, &lightsData[1 * uniformSize]);

		uniform = Common::String::format("lightsColor");
		uniformPos = _actorLightsProgram->getUniformLocation(uniform.c_str());
		if (uniformPos == -1) {
			error("No uniform named '%s'", uniform.c_str());
		}
		metalUniform4fv(uniformPos, _maxLights, &lightsData[2 * uniformSize]);

		uniform = Common::String::format("lightsParams");
		uniformPos = _actorLightsProgram->getUniformLocation(uniform.c_str());
		if (uniformPos == -1) {
			error("No uniform named '%s'", uniform.c_str());
		}
		metalUniform4fv(uniformPos, _maxLights, &lightsData[3 * uniformSize]);

		delete[] lightsData;
	}
}

void GfxMetal::finishActorDraw() {
	_currentActor = nullptr;
	metalDisable(kPolygonOffsetFill);
	if (g_grim->getGameType() == GType_MONKEY4) {
		metalDisable(kCullFace);
	}
}

void GfxMetal::setShadow(Shadow *shadow) {
	_currentShadowArray = shadow;
}

void GfxMetal::drawShadowPlanes() {
	metalColorMask(kFalse, kFalse, kFalse, kFalse);
	metalDepthMask(kFalse);
	metalClearStencil(~0);
	metalClear(kStencilBufferBit);

	metalEnable(kStencilTest);
	metalStencilFunc(kAlways, 1, (uint32)~0);
	metalStencilOp(kReplace, kReplace, kReplace);

	if (!_currentShadowArray->userData) {
		uint32 numVertices = 0;
		uint32 numTriangles = 0;
		for (SectorListType::iterator i = _currentShadowArray->planeList.begin(); i != _currentShadowArray->planeList.end(); ++i) {
			numVertices += i->sector->getNumVertices();
			numTriangles += i->sector->getNumVertices() - 2;
		}

		float *vertBuf = new float[3 * numVertices];
		uint16 *idxBuf = new uint16[3 * numTriangles];

		float *vert = vertBuf;
		uint16 *idx = idxBuf;

		for (SectorListType::iterator i = _currentShadowArray->planeList.begin(); i != _currentShadowArray->planeList.end(); ++i) {
			Sector *shadowSector = i->sector;
			memcpy(vert, shadowSector->getVertices(), 3 * shadowSector->getNumVertices() * sizeof(float));
			uint16 first = (vert - vertBuf) / 3;
			for (uint16 j = 2; j < shadowSector->getNumVertices(); ++j) {
				*idx++ = first;
				*idx++ = first + j - 1;
				*idx++ = first + j;
			}
			vert += 3 * shadowSector->getNumVertices();
		}

		ShadowUserData *sud = new ShadowUserData;
		_currentShadowArray->userData = sud;
		sud->_numTriangles = numTriangles;
		sud->_verticesVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, 3 * numVertices * sizeof(float), vertBuf, kStaticDraw);
		sud->_indicesVBO = MetalBackend::Shader::createBuffer(kElementArrayBuffer, 3 * numTriangles * sizeof(uint16), idxBuf, kStaticDraw);

		delete[] vertBuf;
		delete[] idxBuf;
	}

	const ShadowUserData *sud = (ShadowUserData *)_currentShadowArray->userData;
	_shadowPlaneProgram->use();
	_shadowPlaneProgram->setUniform("projMatrix", _projMatrix);
	_shadowPlaneProgram->setUniform("viewMatrix", _viewMatrix);

	metalBindBuffer(kArrayBuffer, sud->_verticesVBO);
	metalBindBuffer(kElementArrayBuffer, sud->_indicesVBO);
	const uint32 attribPos = _shadowPlaneProgram->getAttribute("position")._idx;
	metalEnableVertexAttribArray(attribPos);
	metalVertexAttribPointer(attribPos, 3, kFloat, kTrue, 3 * sizeof(float), nullptr);
	metalDrawElements(kTriangles, 3 * sud->_numTriangles, kUnsignedShort, nullptr);

	metalColorMask(kTrue, kTrue, kTrue, kTrue);

	metalStencilFunc(kEqual, 1, (uint32)~0);
	metalStencilOp(kKeep, kKeep, kKeep);
}

void GfxMetal::setShadowMode() {
	GfxBase::setShadowMode();
}

void GfxMetal::clearShadowMode() {
	GfxBase::clearShadowMode();

	metalDisable(kStencilTest);
	metalDepthMask(kTrue);
}

void GfxMetal::setShadowColor(byte r, byte g, byte b) {
	_shadowColorR = r;
	_shadowColorG = g;
	_shadowColorB = b;
}

void GfxMetal::getShadowColor(byte *r, byte *g, byte *b) {
	*r = _shadowColorR;
	*g = _shadowColorG;
	*b = _shadowColorB;
}

void GfxMetal::destroyShadow(Shadow *shadow) {
	ShadowUserData *sud = static_cast<ShadowUserData *>(shadow->userData);
	if (sud) {
		MetalBackend::Shader::freeBuffer(sud->_verticesVBO);
		MetalBackend::Shader::freeBuffer(sud->_indicesVBO);
		delete sud;
	}

	shadow->userData = nullptr;
}

void GfxMetal::set3DMode() {
}

void GfxMetal::translateViewpointStart() {
	_matrixStack.push(_matrixStack.top());
}

void GfxMetal::translateViewpoint(const Math::Vector3d &vec) {
	Math::Matrix4 temp;
	temp.setPosition(vec);
	temp.transpose();
	_matrixStack.top() = temp * _matrixStack.top();
}

void GfxMetal::rotateViewpoint(const Math::Angle &angle, const Math::Vector3d &axis_) {
	Math::Matrix4 temp = metalMakeRotationMatrix(angle, axis_) * _matrixStack.top();
	_matrixStack.top() = temp;
}

void GfxMetal::rotateViewpoint(const Math::Matrix4 &rot) {
	Math::Matrix4 temp = rot * _matrixStack.top();
	_matrixStack.top() = temp;
}

void GfxMetal::translateViewpointFinish() {
	_matrixStack.pop();
}

void GfxMetal::updateEMIModel(const EMIModel* model) {
	const EMIModelUserData *mud = (const EMIModelUserData *)model->_userData;
	metalBindBuffer(kArrayBuffer, mud->_verticesVBO);
	metalBufferSubData(kArrayBuffer, 0, model->_numVertices * 3 * sizeof(float), model->_drawVertices);
	metalBindBuffer(kArrayBuffer, mud->_normalsVBO);
	metalBufferSubData(kArrayBuffer, 0, model->_numVertices * 3 * sizeof(float), model->_drawNormals);
}

void GfxMetal::drawEMIModelFace(const EMIModel* model, const EMIMeshFace* face) {
	if (face->_flags & EMIMeshFace::kAlphaBlend || face->_flags & EMIMeshFace::kUnknownBlend)
		metalEnable(kBlend);
	const EMIModelUserData *mud = (const EMIModelUserData *)model->_userData;
	MetalBackend::Shader *actorShader;
	if ((face->_flags & EMIMeshFace::kNoLighting || isShadowModeActive()) ? false : _lightsEnabled)
		actorShader = mud->_shaderLights;
	else
		actorShader = mud->_shader;
	actorShader->use();
	bool textured = face->_hasTexture && !_currentShadowArray;
	actorShader->setUniform("textured", textured ? kTrue : kFalse);
	actorShader->setUniform("useVertexAlpha", _selectedTexture->_hasAlpha);
	actorShader->setUniform1f("meshAlpha", (model->_meshAlphaMode == Actor::AlphaReplace) ? model->_meshAlpha : 1.0f);

	metalBindBuffer(kElementArrayBuffer, face->_indicesEBO);

	metalDrawElements(kTriangles, 3 * face->_faceLength, kUnsignedShort, nullptr);
	metalBindBuffer(kElementArrayBuffer, 0);
}

void GfxMetal::drawMesh(const Mesh *mesh) {
	const ModelUserData *mud = (const ModelUserData *)mesh->_userData;
	if (!mud)
		return;
	MetalBackend::Shader *actorShader;
	if (_lightsEnabled && !isShadowModeActive())
		actorShader = mud->_shaderLights;
	else
		actorShader = mud->_shader;

	actorShader->use();
	actorShader->setUniform("extraMatrix", _matrixStack.top());

	const Material *curMaterial = nullptr;
	for (int i = 0; i < mesh->_numFaces;) {
		const MeshFace *face = &mesh->_faces[i];

		curMaterial = face->getMaterial();
		curMaterial->select();

		int faces = 0;
		for (; i < mesh->_numFaces; ++i) {
			if (mesh->_faces[i].getMaterial() != curMaterial)
				break;
			faces += 3 * (mesh->_faces[i].getNumVertices() - 2);
		}

		bool textured = face->hasTexture() && !_currentShadowArray;
		actorShader->setUniform("textured", textured ? kTrue : kFalse);
		actorShader->setUniform("texScale", Math::Vector2d(_selectedTexture->_width, _selectedTexture->_height));

		metalDrawArrays(kTriangles, *(int *)face->_userData, faces);
	}
}

void GfxMetal::drawDimPlane() {
	if (_dimLevel == 0.0f)
		return;

	metalDisable(kDepthTest);
	metalDepthMask(kFalse);
	metalEnable(kBlend);
	metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);

	_dimPlaneProgram->use();
	_dimPlaneProgram->setUniform1f("dim", _dimLevel);

	metalBindBuffer(kElementArrayBuffer, _quadEBO);
	metalDrawElements(kTriangles, 6, kUnsignedShort, nullptr);
	metalBindBuffer(kElementArrayBuffer, 0);

	metalEnable(kDepthTest);
	metalDepthMask(kTrue);
}

void GfxMetal::drawModelFace(const Mesh *mesh, const MeshFace *face) {
}

void GfxMetal::drawSprite(const Sprite *sprite) {
	if (g_grim->getGameType() == GType_MONKEY4) {
		metalDepthMask(kTrue);
	} else {
		metalDepthMask(kFalse);
	}

	if (sprite->_flags1 & Sprite::BlendAdditive) {
		metalBlendFunc(kSrcAlpha, kOne);
	} else {
		metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
	}

	if (sprite->_flags2 & Sprite::DepthTest || _currentActor->isInOverworld()) {
		metalEnable(kDepthTest);
	} else {
		metalDisable(kDepthTest);
	}

	_spriteProgram->use();

	Math::Matrix4 rotateMatrix;
	rotateMatrix.buildAroundZ(_currentActor->getYaw());

	Math::Matrix4 extraMatrix;
	extraMatrix.setPosition(sprite->_pos);
	extraMatrix(0, 0) = sprite->_width;
	extraMatrix(1, 1) = sprite->_height;

	extraMatrix = rotateMatrix * extraMatrix;
	extraMatrix.transpose();
	_spriteProgram->setUniform("extraMatrix", extraMatrix);
	_spriteProgram->setUniform("textured", kTrue);
	if (g_grim->getGameType() == GType_GRIM) {
		_spriteProgram->setUniform1f("alphaRef", 0.5f);
	} else if (sprite->_flags2 & Sprite::AlphaTest) {
		_spriteProgram->setUniform1f("alphaRef", 0.1f);
	} else {
		_spriteProgram->setUniform1f("alphaRef", 0.0f);
	}

	// FIXME: Currently vertex-specific colors are not supported for sprites.
	// It is unknown at this time if this is really needed anywhere.
	Math::Vector4d color(sprite->_red[0] / 255.0f, sprite->_green[0] / 255.0f, sprite->_blue[0] / 255.0f, sprite->_alpha[0] / 255.0f);
	if (g_grim->getGameType() == GType_MONKEY4) {
		// Don't overwrite the actor's alpha value  which is set via uniformColor in startActorDraw()
		_spriteProgram->setUniform("spriteColor", color);
	} else {
		// No need to set spriteColor for GRIM, it is specific to the EMI sprite shader program
		_spriteProgram->setUniform("uniformColor", color);
	}

	float sprite_textured_quad[] = {
	//	 X   ,  Y   , Z   , S   , T
		-0.5f, +0.5f, 0.0f, sprite->_texCoordX[0], sprite->_texCoordY[0],
		+0.5f, +0.5f, 0.0f, sprite->_texCoordX[1], sprite->_texCoordY[1],
		+0.5f, -0.5f, 0.0f, sprite->_texCoordX[2], sprite->_texCoordY[2],
		-0.5f, -0.5f, 0.0f, sprite->_texCoordX[3], sprite->_texCoordY[3],
	};

	metalBindBuffer(kArrayBuffer, _spriteVBO);
	metalBufferData(kArrayBuffer, sizeof(sprite_textured_quad), sprite_textured_quad, kStaticDraw);
	metalBindBuffer(kElementArrayBuffer, _quadEBO);
	metalDrawElements(kTriangles, 6, kUnsignedShort, nullptr);
	metalBindBuffer(kElementArrayBuffer, 0);
	metalEnable(kDepthTest);
	metalDepthMask(kTrue);
	metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
}

void GfxMetal::enableLights() {
	_lightsEnabled = true;
}

void GfxMetal::disableLights() {
	_lightsEnabled = false;
}

void GfxMetal::setupLight(Grim::Light *light, int lightId) {
	_lightsEnabled = true;

	if (lightId >= _maxLights) {
		return;
	}

	// Disable previous lights.
	if (lightId == 0) {
		_hasAmbientLight = false;
		for (int id = 0; id < _maxLights; ++id)
			_lights[id]._color.w() = 0.0;
	}

	Math::Vector4d &lightColor  = _lights[lightId]._color;
	Math::Vector4d &lightPos    = _lights[lightId]._position;
	Math::Vector4d &lightDir    = _lights[lightId]._direction;
	Math::Vector4d &lightParams = _lights[lightId]._params;

	lightColor.x() = (float)light->_color.getRed();
	lightColor.y() = (float)light->_color.getGreen();
	lightColor.z() = (float)light->_color.getBlue();
	lightColor.w() = light->_scaledintensity;

	if (light->_type == Grim::Light::Omni) {
		lightPos = Math::Vector4d(light->_pos.x(), light->_pos.y(), light->_pos.z(), 1.0f);
		lightDir = Math::Vector4d(0.0f, 0.0f, 0.0f, -1.0f);
		lightParams = Math::Vector4d(light->_falloffNear, light->_falloffFar, 0.0f, 0.0f);
	} else if (light->_type == Grim::Light::Direct) {
		lightPos = Math::Vector4d(-light->_dir.x(), -light->_dir.y(), -light->_dir.z(), 0.0f);
		lightDir = Math::Vector4d(0.0f, 0.0f, 0.0f, -1.0f);
	} else if (light->_type == Grim::Light::Spot) {
		lightPos = Math::Vector4d(light->_pos.x(), light->_pos.y(), light->_pos.z(), 1.0f);
		lightDir = Math::Vector4d(light->_dir.x(), light->_dir.y(), light->_dir.z(), 1.0f);
		lightParams = Math::Vector4d(light->_falloffNear, light->_falloffFar, light->_cospenumbraangle, light->_cosumbraangle);
	} else if (light->_type == Grim::Light::Ambient) {
		lightPos = Math::Vector4d(0.0f, 0.0f, 0.0f, -1.0f);
		lightDir = Math::Vector4d(0.0f, 0.0f, 0.0f, -1.0f);
		_hasAmbientLight = true;
	}
}

void GfxMetal::turnOffLight(int lightId) {
	if (lightId >= _maxLights) {
		return;
	}

	_lights[lightId]._color = Math::Vector4d(0.0f, 0.0f, 0.0f, 0.0f);
	_lights[lightId]._position = Math::Vector4d(0.0f, 0.0f, 0.0f, 0.0f);
	_lights[lightId]._direction = Math::Vector4d(0.0f, 0.0f, 0.0f, 0.0f);
}


void GfxMetal::createTexture(Texture *texture, const uint8 *data, const CMap *cmap, bool clamp) {
	texture->_texture = new uint32[1];
	metalGenTextures(1, (uint32 *)texture->_texture);

	uint32 *textures = (uint32 *)texture->_texture;
	metalBindTexture(kTexture2D, textures[0]);

	// Remove darkened lines in EMI intro
	if (g_grim->getGameType() == GType_MONKEY4 && clamp) {
		metalTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
		metalTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
	} else {
		metalTexParameteri(kTexture2D, kTextureWrapS, kRepeat);
		metalTexParameteri(kTexture2D, kTextureWrapT, kRepeat);
	}

	metalTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
	metalTexParameteri(kTexture2D, kTextureMinFilter, kLinear);

	if (cmap != nullptr) { // EMI doesn't have colour-maps
		int bytes = 4;

		char *texdata = new char[texture->_width * texture->_height * bytes];
		char *texdatapos = texdata;

		for (int y = 0; y < texture->_height; y++) {
			for (int x = 0; x < texture->_width; x++) {
				uint8 col = *(const uint8 *)(data);
				if (col == 0) {
					memset(texdatapos, 0, bytes); // transparent
					if (!texture->_hasAlpha) {
						texdatapos[3] = '\xff'; // fully opaque
					}
				} else {
					memcpy(texdatapos, cmap->_colors + 3 * (col), 3);
					texdatapos[3] = '\xff'; // fully opaque
				}
				texdatapos += bytes;
				data++;
			}
		}

		metalTexImage2D(kTexture2D, 0, kRgba, texture->_width, texture->_height, 0, kRgba, kUnsignedByte, texdata);
		delete[] texdata;
	} else {
		int format = (texture->_bpp == 4) ? kRgba : kRgb;
		metalTexImage2D(kTexture2D, 0, format, texture->_width, texture->_height, 0, format, kUnsignedByte, data);
	}
}

void GfxMetal::selectTexture(const Texture *texture) {
	uint32 *textures = (uint32 *)texture->_texture;
	metalBindTexture(kTexture2D, textures[0]);

	if (texture->_hasAlpha && g_grim->getGameType() == GType_MONKEY4) {
		metalEnable(kBlend);
	}

	_selectedTexture = const_cast<Texture *>(texture);
}

void GfxMetal::destroyTexture(Texture *texture) {
	uint32 *textures = static_cast<uint32 *>(texture->_texture);
	if (textures) {
		metalDeleteTextures(1, textures);
		delete[] textures;
	}
}

void GfxMetal::createBitmap(BitmapData *bitmap) {
	if (bitmap->_format != 1) {
		for (int pic = 0; pic < bitmap->_numImages; pic++) {
			uint16 *zbufPtr = reinterpret_cast<uint16 *>(const_cast<void *>(bitmap->getImageData(pic).getPixels()));
			for (int i = 0; i < (bitmap->_width * bitmap->_height); i++) {
				uint16 val = zbufPtr[i];
				// fix the value if it is incorrectly set to the bitmap transparency color
				if (val == 0xf81f) {
					val = 0;
				}
				// This is later read as a LA pair when filling texture
				// with L being used as the LSB in fragment shader
				zbufPtr[i] = TO_LE_16(0xffff - ((uint32)val) * 0x10000 / 100 / (0x10000 - val));
			}
		}
	}

	bitmap->_hasTransparency = false;
	if (bitmap->_format == 1) {
		bitmap->_numTex = 1;
		uint32 *textures = new uint32[bitmap->_numTex * bitmap->_numImages];
		bitmap->_texIds = textures;
		metalGenTextures(bitmap->_numTex * bitmap->_numImages, textures);

		byte *texData = nullptr;
		const byte *texOut = nullptr;

		int format = kRgba;
		int btype = kUnsignedByte;
		int bytes = 4;

		metalPixelStorei(kUnpackAlignment, bytes);

		const Graphics::PixelFormat format_16bpp(2, 5, 6, 5, 0, 11, 5, 0, 0);
		const Graphics::PixelFormat format_32bpp = Graphics::PixelFormat::createFormatRGBA32();

		for (int pic = 0; pic < bitmap->_numImages; pic++) {
			const Graphics::Surface &imageData = bitmap->getImageData(pic);
			if (bitmap->_format == 1 && imageData.format == format_16bpp) {
				if (texData == nullptr)
					texData = new byte[bytes * bitmap->_width * bitmap->_height];
				// Convert data to 32-bit RGBA format
				byte *texDataPtr = texData;
				const uint16 *bitmapData = reinterpret_cast<const uint16 *>(imageData.getPixels());
				for (int i = 0; i < bitmap->_width * bitmap->_height; i++, texDataPtr += bytes, bitmapData++) {
					uint16 pixel = *bitmapData;
					int r = pixel >> 11;
					texDataPtr[0] = (r << 3) | (r >> 2);
					int g = (pixel >> 5) & 0x3f;
					texDataPtr[1] = (g << 2) | (g >> 4);
					int b = pixel & 0x1f;
					texDataPtr[2] = (b << 3) | (b >> 2);
					if (pixel == 0xf81f) { // transparent
						texDataPtr[3] = 0;
						bitmap->_hasTransparency = true;
					} else {
						texDataPtr[3] = 255;
					}
				}
				texOut = texData;
			} else if (bitmap->_format == 1 && imageData.format != format_32bpp) {
				bitmap->convertToColorFormat(pic, format_32bpp);
				texOut = (const byte *)imageData.getPixels();
			} else {
				texOut = (const byte *)imageData.getPixels();
			}

			int actualWidth = nextHigher2(bitmap->_width);
			int actualHeight = nextHigher2(bitmap->_height);

			metalBindTexture(kTexture2D, textures[bitmap->_numTex * pic]);
			metalTexParameteri(kTexture2D, kTextureMagFilter, kNearest);
			metalTexParameteri(kTexture2D, kTextureMinFilter, kNearest);
			metalTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
			metalTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
			metalTexImage2D(kTexture2D, 0, format, actualWidth, actualHeight, 0, format, btype, nullptr);

			metalTexSubImage2D(kTexture2D, 0, 0, 0, bitmap->_width, bitmap->_height, format, btype, texOut);
		}

		if (texData)
			delete[] texData;
		bitmap->freeData();

		MetalBackend::Shader *shader = _backgroundProgram->clone();
		bitmap->_userData = shader;

		if (g_grim->getGameType() == GType_MONKEY4) {
			uint32 vbo = MetalBackend::Shader::createBuffer(kArrayBuffer, bitmap->_numCoords * 4 * sizeof(float), bitmap->_texc, kStaticDraw);
			shader->enableVertexAttribute("position", vbo, 2, kFloat, kFalse, 4 * sizeof(float), 0);
			shader->enableVertexAttribute("texcoord", vbo, 2, kFloat, kFalse, 4 * sizeof(float), 2*sizeof(float));
		}
	} else {
		bitmap->_numTex = 0;
		bitmap->_texIds = nullptr;
		bitmap->_userData = nullptr;
	}
}

void GfxMetal::drawBitmap(const Bitmap *bitmap, int dx, int dy, uint32 layer) {
	if (g_grim->getGameType() == GType_MONKEY4 && bitmap->_data && bitmap->_data->_texc) {
		BitmapData *data = bitmap->_data;
		MetalBackend::Shader *shader = (MetalBackend::Shader *)data->_userData;
		uint32 *textures = (uint32 *)bitmap->getTexIds();

		metalDisable(kDepthTest);

		metalEnable(kBlend);
		metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);

		shader->use();
		metalBindBuffer(kElementArrayBuffer, _quadEBO);
		assert(layer < data->_numLayers);
		uint32 offset = data->_layers[layer]._offset;
		for (uint32 i = offset; i < offset + data->_layers[layer]._numImages; ++i) {
			metalBindTexture(kTexture2D, textures[data->_verts[i]._texid]);

			unsigned short startVertex = data->_verts[i]._pos / 4 * 6;
			unsigned short numVertices = data->_verts[i]._verts / 4 * 6;
			metalDrawElements(kTriangles, numVertices, kUnsignedShort, (void *)(startVertex * sizeof(unsigned short)));
		}
		return;
	}

	int format = bitmap->getFormat();
	if ((format == 1 && !_renderBitmaps) || (format == 5 && !_renderZBitmaps)) {
		return;
	}

	if (format == 1) {
		uint32 *textures = (uint32 *)bitmap->getTexIds();
		if (bitmap->getFormat() == 1 && bitmap->getHasTransparency()) {
			metalEnable(kBlend);
			metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
		} else {
			metalDisable(kBlend);
		}

		MetalBackend::Shader *shader = (MetalBackend::Shader *)bitmap->_data->_userData;
		shader->use();
		metalDisable(kDepthTest);
		metalDepthMask(kFalse);

		metalBindBuffer(kElementArrayBuffer, _quadEBO);
		int cur_tex_idx = bitmap->getNumTex() * (bitmap->getActiveImage() - 1);
		metalBindTexture(kTexture2D, textures[cur_tex_idx]);
		float width = bitmap->getWidth();
		float height = bitmap->getHeight();
		shader->setUniform("offsetXY", Math::Vector2d(float(dx) / _gameWidth, float(dy) / _gameHeight));
		shader->setUniform("sizeWH", Math::Vector2d(width / _gameWidth, height / _gameHeight));
		shader->setUniform("texcrop", Math::Vector2d(width / nextHigher2((int)width), height / nextHigher2((int)height)));
		metalDrawElements(kTriangles, 6, kUnsignedShort, nullptr);

		metalDisable(kBlend);
		metalDepthMask(kTrue);
		metalEnable(kDepthTest);
	} else {
		// Only draw the manual zbuffer when enabled
		if (bitmap->getActiveImage() - 1 < bitmap->getNumImages()) {
			drawDepthBitmap(bitmap->getId(), dx, dy, bitmap->getWidth(), bitmap->getHeight(),
			                (char *)const_cast<void *>(bitmap->getData(bitmap->getActiveImage() - 1).getPixels()));
		} else {
			warning("zbuffer image has index out of bounds! %d/%d", bitmap->getActiveImage(), bitmap->getNumImages());
		}
		return;
	}
}

void GfxMetal::drawDepthBitmap(int bitmapId, int x, int y, int w, int h, char *data) {
	// Sometimes the data pointer is reused by the allocator between bitmaps
	// Use the bitmap ID to ensure we don't prevent an expected update
	Common::Rect rect(x, y, x + w, y + h);
	if (bitmapId == _depthBitmapId && _depthBitmapRect == rect && _depthBitmapData == data) {
		return;
	}
	_depthBitmapId = bitmapId;
	_depthBitmapRect = rect;
	_depthBitmapData = data;

	metalActiveTexture(kTexture1);
	metalBindTexture(kTexture2D, _zBufTex);
	metalPixelStorei(kUnpackAlignment, 2); // 16 bit Z depth bitmap
	metalTexSubImage2D(kTexture2D, 0, x, y, w, h, kLuminanceAlpha, kUnsignedByte, data);
	metalPixelStorei(kUnpackAlignment, 4);
	metalActiveTexture(kTexture0);
}

void GfxMetal::destroyBitmap(BitmapData *bitmap) {
	uint32 *textures = (uint32 *)bitmap->_texIds;
	if (textures) {
		metalDeleteTextures(bitmap->_numTex * bitmap->_numImages, textures);
		delete[] textures;
		bitmap->_texIds = nullptr;
	}
	MetalBackend::Shader *shader = (MetalBackend::Shader *)bitmap->_userData;
	if (g_grim->getGameType() == GType_MONKEY4) {
		metalDeleteBuffers(1, &shader->getAttributeAt(0)._vbo);
	}
	delete shader;

	if (bitmap->_format != 1) {
		bitmap->freeData();
	}
}

void GfxMetal::createFont(Font *f) {
	if (!f->is8Bit())
		return;
	BitmapFont *font = static_cast<BitmapFont *>(f);
	const byte *bitmapData = font->getFontData();
	uint dataSize = font->getDataSize();

	uint8 bpp = 4;
	uint8 charsWide = 16;
	uint8 charsHigh = 16;

	byte *texDataPtr = new byte[dataSize * bpp];
	byte *data = texDataPtr;

	for (uint i = 0; i < dataSize; i++, texDataPtr += bpp, bitmapData++) {
		byte pixel = *bitmapData;
		if (pixel == 0x00) {
			texDataPtr[0] = texDataPtr[1] = texDataPtr[2] = texDataPtr[3] = 0;
		} else if (pixel == 0x80) {
			texDataPtr[0] = texDataPtr[1] = texDataPtr[2] = 0;
			texDataPtr[3] = 255;
		} else if (pixel == 0xFF) {
			texDataPtr[0] = texDataPtr[1] = texDataPtr[2] = texDataPtr[3] = 255;
		}
	}
	int size = 0;
	for (int i = 0; i < 256; ++i) {
		int width = font->getCharBitmapWidth(i), height = font->getCharBitmapHeight(i);
		int m = MAX(width, height);
		if (m > size)
			size = m;
	}
	assert(size < 64);
	if (size < 8)
		size = 8;
	if (size < 16)
		size = 16;
	else if (size < 32)
		size = 32;
	else if (size < 64)
		size = 64;

	uint arraySize = size * size * bpp * charsWide * charsHigh;
	byte *temp = new byte[arraySize]();

	FontUserData *userData = new FontUserData;
	font->setUserData(userData);
	userData->texture = 0;
	userData->size = size;

	uint32 *texture = &(userData->texture);
	metalGenTextures(1, texture);

	for (int i = 0, row = 0; i < 256; ++i) {
		int width = font->getCharBitmapWidth(i), height = font->getCharBitmapHeight(i);
		int32 d = font->getCharOffset(i);
		for (int x = 0; x < height; ++x) {
			// a is the offset to get to the correct row.
			// b is the offset to get to the correct line in the character.
			// c is the offset of the character from the start of the row.
			uint a = row * size * size * bpp * charsHigh;
			uint b = x * size * charsWide * bpp;
			uint c = 0;
			if (i != 0)
				c = ((i - 1) % 16) * size * bpp;

			uint pos = a + b + c;
			uint pos2 = d * bpp + x * width * bpp;
			assert(pos + width * bpp <= arraySize);
			assert(pos2 + width * bpp <= dataSize * bpp);
			memcpy(temp + pos, data + pos2, width * bpp);
		}
		if (i != 0 && i % charsWide == 0)
			++row;
	}

	metalBindTexture(kTexture2D, texture[0]);
	metalTexParameteri(kTexture2D, kTextureMagFilter, kNearest);
	metalTexParameteri(kTexture2D, kTextureMinFilter, kNearest);
	metalTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
	metalTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
	metalTexImage2D(kTexture2D, 0, kRgba, size * charsWide, size * charsHigh, 0, kRgba, kUnsignedByte, temp);

	delete[] data;
	delete[] temp;
}

void GfxMetal::destroyFont(Font *font) {
	if (font->is8Bit()) {
		const FontUserData *data = static_cast<const FontUserData *>(static_cast<const BitmapFont *>(font)->getUserData());
		if (data) {
			metalDeleteTextures(1, &(data->texture));
			delete data;
		}
	}
}

void GfxMetal::createTextObject(TextObject *text) {
	const Color &color = text->getFGColor();
	const Font *f = text->getFont();
	if (!f->is8Bit()) {
		TextUserData *data = new TextUserData;
		data->color = color;
		const Graphics::PixelFormat format = Graphics::PixelFormat::createFormatRGBA32();
		for (int line = 0; line < text->getNumLines(); ++line) {
			Graphics::Surface surface;
			f->render(surface, text->getLines()[line], format, format.RGBToColor(0, 0, 0),
				format.RGBToColor(255, 255, 255), 0);
			uint32 image = 0;
			if (surface.w && surface.h) {
				metalGenTextures(1, &image);
				metalBindTexture(kTexture2D, image);
				metalTexParameteri(kTexture2D, kTextureMinFilter, kNearest);
				metalTexParameteri(kTexture2D, kTextureMagFilter, kNearest);
				metalTexImage2D(kTexture2D, 0, kRgba, surface.w, surface.h, 0, kRgba, kUnsignedByte, surface.getPixels());
			}
			data->lineTextures.push_back(image);
			data->lineSizes.push_back(Math::Vector2d(surface.w, surface.h));
			surface.free();
		}
		text->setUserData(data);
		return;
	}
	const BitmapFont *font = static_cast<const BitmapFont *>(f);

	const FontUserData *userData = (const FontUserData *)font->getUserData();
	if (!userData)
		error("Could not get font userdata");

	float sizeW = float(userData->size) / _gameWidth;
	float sizeH = float(userData->size) / _gameHeight;
	const Common::String *lines = text->getLines();
	int numLines = text->getNumLines();

	int numCharacters = 0;
	for (int j = 0; j < numLines; ++j) {
		numCharacters += lines[j].size();
	}

	float *bufData = new float[numCharacters * 16];
	float *cur = bufData;

	for (int j = 0; j < numLines; ++j) {
		const Common::String &line = lines[j];
		int x = text->getLineX(j);
		int y = text->getLineY(j);
		for (uint i = 0; i < line.size(); ++i) {
			uint8 character = line[i];
			float w = y + font->getCharStartingLine(character);
			if (g_grim->getGameType() == GType_GRIM)
				w += font->getBaseOffsetY();
			float z = x + font->getCharStartingCol(character);
			z /= _gameWidth;
			w /= _gameHeight;
			float width = 1 / 16.f;
			float cx = ((character - 1) % 16) / 16.0f;
			float cy = ((character - 1) / 16) / 16.0f;

			float charData[] = {
				z, w, cx, cy,
				z + sizeW, w, cx + width, cy,
				z + sizeW, w + sizeH, cx + width, cy + width,
				z, w + sizeH, cx, cy + width
			};
			memcpy(cur, charData, 16 * sizeof(float));
			cur += 16;

			x += font->getCharKernedWidth(character);
		}
	}
	uint32 vbo;
	bool ownsVertexBuffer = !text->isBlastDraw() || numCharacters > 128;
	if (!ownsVertexBuffer) {
		vbo = _blastVBO;
		metalBindBuffer(kArrayBuffer, vbo);
		metalBufferSubData(kArrayBuffer, 0, numCharacters * 16 * sizeof(float), bufData);
	} else {
		vbo = MetalBackend::Shader::createBuffer(kArrayBuffer, numCharacters * 16 * sizeof(float), bufData, kStaticDraw);
	}

	MetalBackend::Shader * textShader = _textProgram->clone();
	metalBindBuffer(kArrayBuffer, vbo);

	textShader->enableVertexAttribute("position", vbo, 2, kFloat, kFalse, 4 * sizeof(float), 0);
	textShader->enableVertexAttribute("texcoord", vbo, 2, kFloat, kFalse, 4 * sizeof(float), 2 * sizeof(float));

	TextUserData * td = new TextUserData;
	td->characters = numCharacters;
	td->shader = textShader;
	td->color = color;
	td->texture = userData->texture;
	td->ownsVertexBuffer = ownsVertexBuffer;
	text->setUserData(td);
	delete[] bufData;
}

void GfxMetal::drawTextObject(const TextObject *text) {
	metalEnable(kBlend);
	metalDisable(kDepthTest);
	const TextUserData * td = (const TextUserData *) text->getUserData();
	assert(td);
	if (!text->getFont()->is8Bit()) {
		Math::Vector3d color(td->color.getRed() / 255.f, td->color.getGreen() / 255.f, td->color.getBlue() / 255.f);
		_textProgram->setUniform("color", color);
		metalDepthMask(false);
		for (uint line = 0; line < td->lineTextures.size(); ++line) {
			if (!td->lineTextures[line]) continue;
			float x = text->getLineX(line), y = text->getLineY(line);
			float width = text->getFont()->getKernedStringLength(text->getLines()[line]);
			float height = text->getFont()->getKernedHeight();
			float scaleW = text->getCoords() == 0 ? _scaleW : _globalScaleW;
			float scaleH = text->getCoords() == 0 ? _scaleH : _globalScaleH;
			x = x * scaleW / _screenWidth; y = y * scaleH / _screenHeight;
			width = width * scaleW / _screenWidth; height = height * scaleH / _screenHeight;
			float vertices[] = {x, y, 0, 0, x + width, y, 1, 0, x + width, y + height, 1, 1, x, y + height, 0, 1};
			uint32 buffer = MetalBackend::Shader::createBuffer(kArrayBuffer, sizeof(vertices), vertices, kStreamDraw);
			MetalBackend::Shader *shader = _textProgram->clone();
			shader->enableVertexAttribute("position", buffer, 2, kFloat, false, 4 * sizeof(float), 0);
			shader->enableVertexAttribute("texcoord", buffer, 2, kFloat, false, 4 * sizeof(float), 2 * sizeof(float));
			shader->use();
			metalBindTexture(kTexture2D, td->lineTextures[line]);
			metalDrawArrays(kTriangleFan, 0, 4);
			delete shader;
			MetalBackend::Shader::freeBuffer(buffer);
		}
		metalDepthMask(true);
		metalEnable(kDepthTest);
		return;
	}
	td->shader->use();

	Math::Vector3d colors(float(td->color.getRed()) / 255.0f,
	                      float(td->color.getGreen()) / 255.0f,
	                      float(td->color.getBlue()) / 255.0f);
	td->shader->setUniform("color", colors);
	metalBindTexture(kTexture2D, td->texture);
	metalBindBuffer(kElementArrayBuffer, _quadEBO);
	metalDrawElements(kTriangles, td->characters * 6, kUnsignedShort, nullptr);
	metalBindBuffer(kElementArrayBuffer, 0);
	metalEnable(kDepthTest);
}

void GfxMetal::destroyTextObject(TextObject *text) {
	const TextUserData * td = (const TextUserData *) text->getUserData();
	if (!td) return;
	if (!td->lineTextures.empty()) metalDeleteTextures(td->lineTextures.size(), td->lineTextures.data());
	if (td->shader && td->ownsVertexBuffer) {
		metalDeleteBuffers(1, &td->shader->getAttributeAt(0)._vbo);
	}
	text->setUserData(nullptr);

	delete td->shader;
	delete td;
}

void GfxMetal::storeDisplay() {
	metalBindTexture(kTexture2D, _storedDisplay);
	metalTexImage2D(kTexture2D, 0, kRgba, _screenWidth, _screenHeight, 0, kRgba, kUnsignedByte, nullptr);
	metalTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
	metalTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
	metalCopyTexImage2D(kTexture2D, 0, kRgba, 0, 0, _screenWidth, _screenHeight, 0);
}

void GfxMetal::copyStoredToDisplay() {
	if (!_dimProgram)
		return;

	_dimProgram->use();
	_dimProgram->setUniform("scaleWH", Math::Vector2d(1.f, 1.f));
	_dimProgram->setUniform("tex", 0);
	_dimProgram->setUniform1f("dim", -1);

	metalBindTexture(kTexture2D, _storedDisplay);

	metalDisable(kDepthTest);
	metalDepthMask(kFalse);

	metalDrawArrays(kTriangles, 0, 6);

	metalEnable(kDepthTest);
	metalDepthMask(kTrue);
}

void GfxMetal::dimScreen() {
	if (!_dimProgram) return;
	_dimProgram->setUniform("scaleWH", Math::Vector2d(1, 1));
	_dimProgram->setUniform1f("dim", 0.1f);
	metalFilterTexture(_storedDisplay, *_dimProgram);
}

void GfxMetal::dimRegion(int xin, int yReal, int w, int h, float level) {
	xin = (int)(xin * _scaleW);
	yReal = (int)(yReal * _scaleH);
	w = (int)(w * _scaleW);
	h = (int)(h * _scaleH);
	int yin = _screenHeight - yReal - h;

	uint32 texture;
	metalGenTextures(1, &texture);
	metalBindTexture(kTexture2D, texture);

	metalTexImage2D(kTexture2D, 0, kRgba, w, h, 0, kRgba, kUnsignedByte, nullptr);
	metalTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
	metalTexParameteri(kTexture2D, kTextureMagFilter, kLinear);

	g_system->presentBuffer();
	metalCopyTexImage2D(kTexture2D, 0, kRgba, xin, yin, w, h, 0);

	metalBindBuffer(kArrayBuffer, _dimRegionVBO);

	float width = w;
	float height = h;
	float x = xin;
	float y = yin;
	float points[24] = {
		x, y, 0.0f, 0.0f,
		x + width, y, 1.0f, 0.0f,
		x + width, y + height, 1.0f, 1.0f,
		x + width, y + height, 1.0f, 1.0f,
		x, y + height, 0.0f, 1.0f,
		x, y, 0.0f, 0.0f,
	};

	metalBufferSubData(kArrayBuffer, 0, 24 * sizeof(float), points);

	_dimRegionProgram->use();
	_dimRegionProgram->setUniform("scaleWH", Math::Vector2d(1.f / _screenWidth, 1.f / _screenHeight));
	_dimRegionProgram->setUniform("tex", 0);
	_dimRegionProgram->setUniform1f("dim", level / 3.f);

	metalDisable(kDepthTest);
	metalDepthMask(kFalse);

	metalDrawArrays(kTriangles, 0, 6);

	metalEnable(kDepthTest);
	metalDepthMask(kTrue);

	metalDeleteTextures(1, &texture);
}

void GfxMetal::irisAroundRegion(int x1, int y1, int x2, int y2) {
	_irisProgram->use();
	_irisProgram->setUniform("color", Math::Vector3d(0.0f, 0.0f, 0.0f));
	_irisProgram->setUniform("scaleWH", Math::Vector2d(1.f / _gameWidth, 1.f / _gameHeight));

	float fx1 = x1;
	float fx2 = x2;
	float fy1 = y1;
	float fy2 = y2;
	float width = _screenWidth;
	float height = _screenHeight;
	float points[20] = {
		0.0f, 0.0f,
		0.0f, fy1,
		width, 0.0f,
		fx2, fy1,
		width, height,
		fx2, fy2,
		0.0f, height,
		fx1, fy2,
		0.0f, fy1,
		fx1, fy1
	};

	metalBindBuffer(kArrayBuffer, _irisVBO);
	metalBufferSubData(kArrayBuffer, 0, 20 * sizeof(float), points);

	metalDisable(kDepthTest);
	metalDepthMask(kFalse);

	metalDrawArrays(kTriangleStrip, 0, 10);

	metalEnable(kDepthTest);
	metalDepthMask(kTrue);
}

void GfxMetal::drawEmergString(int x, int y, const char *text, const Color &fgColor) {
	if (!*text)
		return;

	metalEnable(kBlend);
	metalDisable(kDepthTest);
	metalBindTexture(kTexture2D, _emergTexture);
	_emergProgram->use();
	Math::Vector3d colors(float(fgColor.getRed()) / 255.0f,
	                      float(fgColor.getGreen()) / 255.0f,
	                      float(fgColor.getBlue()) / 255.0f);
	_emergProgram->setUniform("color", colors);
	_emergProgram->setUniform("sizeWH", Math::Vector2d(float(8) / _gameWidth, float(16) / _gameHeight));
	_emergProgram->setUniform("texScale", Math::Vector2d(float(8) / 128, float(16) / 128));

	for (; *text; ++text, x+=10) {
		int blockcol = *text & 0xf;
		int blockrow = *text / 16;
		_emergProgram->setUniform("offsetXY", Math::Vector2d(float(x) / _gameWidth, float(y) / _gameHeight));
		_emergProgram->setUniform("texOffsetXY", Math::Vector2d(float(blockcol * 8) / 128, float(blockrow * 16) / 128));
		metalDrawArrays(kTriangleFan, 0, 4);
	}
}

void GfxMetal::loadEmergFont() {
	uint8 *atlas = new uint8[128 * 128]();

	for (int c = 32; c < 128; ++c) {
		int blockrow = c / 16;
		int blockcol = c & 0xf;
		for (int row = 0; row < 13; ++row) {
			int base = 128 * (16 * blockrow + row) + 8 * blockcol;
			uint8 val = BitmapFont::emerFont[c - 32][row];
			atlas[base + 0] = (val & 0x80) ? 255 : 0;
			atlas[base + 1] = (val & 0x40) ? 255 : 0;
			atlas[base + 2] = (val & 0x20) ? 255 : 0;
			atlas[base + 3] = (val & 0x10) ? 255 : 0;
			atlas[base + 4] = (val & 0x08) ? 255 : 0;
			atlas[base + 5] = (val & 0x04) ? 255 : 0;
			atlas[base + 6] = (val & 0x02) ? 255 : 0;
			atlas[base + 7] = (val & 0x01) ? 255 : 0;
		}
	}

	metalGenTextures(1, &_emergTexture);
	metalBindTexture(kTexture2D, _emergTexture);
	metalTexParameteri(kTexture2D, kTextureMagFilter, kLinear);
	metalTexParameteri(kTexture2D, kTextureMinFilter, kLinear);
	metalTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
	metalTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
	metalTexImage2D(kTexture2D, 0, kAlpha, 128, 128, 0, kAlpha, kUnsignedByte, atlas);

	delete[] atlas;
}

void GfxMetal::drawGenericPrimitive(const float *vertices, uint32 numVertices, const PrimitiveObject *primitive) {
	const Color color(primitive->getColor());
	const Math::Vector3d colorV =
	  Math::Vector3d(color.getRed(), color.getGreen(), color.getBlue()) / 255.f;
	if (primitive->getType() == PrimitiveObject::LineType || primitive->getType() == PrimitiveObject::PolygonType) {
		Graphics::Metal::DrawState state;
		Graphics::Metal::Uniforms uniforms;
		for (uint i = 0; i + 3 < numVertices; i += 4) {
			Graphics::Metal::Vertex start = {{2 * vertices[i] / _screenWidth - 1, 1 - 2 * vertices[i + 1] / _screenHeight, 0}, {0, 0}, {colorV.x(), colorV.y(), colorV.z(), 1}};
			Graphics::Metal::Vertex end = {{2 * vertices[i + 2] / _screenWidth - 1, 1 - 2 * vertices[i + 3] / _screenHeight, 0}, {0, 0}, {colorV.x(), colorV.y(), colorV.z(), 1}};
			g_system->getMetalRenderer()->drawWideLine(start, end, _scaleW, state, uniforms);
		}
		return;
	}

	uint32 prim = nextPrimitive();
	metalBindBuffer(kArrayBuffer, prim);
	metalBufferSubData(kArrayBuffer, 0, numVertices * sizeof(float), vertices);

	metalDisable(kDepthTest);
	metalDepthMask(kFalse);

	_primitiveProgram->enableVertexAttribute("position", prim, 2, kFloat, kFalse, 2 * sizeof(float), 0);
	_primitiveProgram->use(true);
	_primitiveProgram->setUniform("color", colorV);
	_primitiveProgram->setUniform("scaleWH", Math::Vector2d(1.f / _screenWidth, 1.f / _screenHeight));

	switch (primitive->getType()) {
		case PrimitiveObject::RectangleType:
			metalDrawArrays(kTriangleStrip, 0, 4);
			break;
		case PrimitiveObject::LineType:
			metalDrawArrays(kLines, 0, 2);
			break;
		case PrimitiveObject::PolygonType:
			metalDrawArrays(kLines, 0, 4);
			break;
		default:
			// Impossible
			break;
	}

	metalBindBuffer(kArrayBuffer, 0);
	metalDepthMask(kTrue);
	metalEnable(kDepthTest);
}

void GfxMetal::drawRectangle(const PrimitiveObject *primitive) {
	float x1 = primitive->getP1().x * _scaleW;
	float y1 = primitive->getP1().y * _scaleH;
	float x2 = primitive->getP2().x * _scaleW;
	float y2 = primitive->getP2().y * _scaleH;

	if (primitive->isFilled()) {
		float data[] = { x1, y1, x2 + 1, y1, x1, y2 + 1, x2 + 1, y2 + 1 };
		drawGenericPrimitive(data, 8, primitive);
	} else {
		float top[] =    { x1, y1, x2 + _scaleW, y1, x1, y1 + _scaleH, x2 + _scaleW, y1 + _scaleH };
		float right[] =  { x2, y1, x2 + _scaleW, y1, x2, y2 + _scaleH, x2 + _scaleW, y2 + _scaleH };
		float bottom[] = { x1, y2, x2 + _scaleW, y2, x1, y2 + _scaleH, x2 + _scaleW, y2 + _scaleH };
		float left[] =   { x1, y1, x1 + _scaleW, y1, x1, y2 + _scaleH, x1 + _scaleW, y2 + _scaleH };
		drawGenericPrimitive(top, 8, primitive);
		drawGenericPrimitive(right, 8, primitive);
		drawGenericPrimitive(bottom, 8, primitive);
		drawGenericPrimitive(left, 8, primitive);
	}
}

void GfxMetal::drawLine(const PrimitiveObject *primitive) {
	float x1 = primitive->getP1().x * _scaleW;
	float y1 = primitive->getP1().y * _scaleH;
	float x2 = primitive->getP2().x * _scaleW;
	float y2 = primitive->getP2().y * _scaleH;

	float data[] = { x1, y1, x2, y2 };

	drawGenericPrimitive(data, 4, primitive);
}

void GfxMetal::drawPolygon(const PrimitiveObject *primitive) {
	float x1 = primitive->getP1().x * _scaleW;
	float y1 = primitive->getP1().y * _scaleH;
	float x2 = primitive->getP2().x * _scaleW;
	float y2 = primitive->getP2().y * _scaleH;
	float x3 = primitive->getP3().x * _scaleW;
	float y3 = primitive->getP3().y * _scaleH;
	float x4 = primitive->getP4().x * _scaleW;
	float y4 = primitive->getP4().y * _scaleH;

	const float data[] = { x1, y1, x2 + 1, y2 + 1, x3, y3 + 1, x4 + 1, y4 };

	drawGenericPrimitive(data, 8, primitive);
}

const Graphics::PixelFormat GfxMetal::getMovieFormat() const {
	return Graphics::PixelFormat::createFormatRGBA32();
}

void GfxMetal::prepareMovieFrame(Graphics::Surface* frame) {
	int width = frame->w;
	int height = frame->h;
	const byte *bitmap = (const byte *)frame->getPixels();

	MetalBackend::StateToken frameType, frameFormat;

	// Used by Bink, QuickTime, MPEG, Theora and paletted SMUSH
	if (frame->format == getMovieFormat()) {
		frameType = kUnsignedByte;
		frameFormat = kRgba;
	// Used by 16-bit SMUSH
	} else if (frame->format == Graphics::PixelFormat(2, 5, 6, 5, 0, 11, 5, 0, 0)) {
		frameType = kUnsignedShort565;
		frameFormat = kRgb;
	} else {
		error("Unknown pixelformat: Bpp: %d RBits: %d GBits: %d BBits: %d ABits: %d RShift: %d GShift: %d BShift: %d AShift: %d",
			frame->format.bytesPerPixel,
			-(frame->format.rLoss - 8),
			-(frame->format.gLoss - 8),
			-(frame->format.bLoss - 8),
			-(frame->format.aLoss - 8),
			frame->format.rShift,
			frame->format.gShift,
			frame->format.bShift,
			frame->format.aShift);
	}

	// create texture
	if (_smushTexId == 0) {
		metalGenTextures(1, &_smushTexId);
	}
	metalBindTexture(kTexture2D, _smushTexId);
	metalTexParameteri(kTexture2D, kTextureMagFilter, kNearest);
	metalTexParameteri(kTexture2D, kTextureMinFilter, kNearest);
	metalTexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
	metalTexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
	metalTexImage2D(kTexture2D, 0, frameFormat, nextHigher2(width), nextHigher2(height), 0, frameFormat, frameType, nullptr);

	metalPixelStorei(kUnpackAlignment, frame->format.bytesPerPixel);
	metalTexSubImage2D(kTexture2D, 0, 0, 0, width, height, frameFormat, frameType, bitmap);
	metalPixelStorei(kUnpackAlignment, 4);

	_smushWidth = (int)(width);
	_smushHeight = (int)(height);
}

void GfxMetal::drawMovieFrame(int offsetX, int offsetY) {
	_smushProgram->use();
	metalDisable(kDepthTest);

	metalBindBuffer(kElementArrayBuffer, _quadEBO);
	_smushProgram->setUniform("texcrop", Math::Vector2d(float(_smushWidth) / nextHigher2(_smushWidth), float(_smushHeight) / nextHigher2(_smushHeight)));
	_smushProgram->setUniform("scale", Math::Vector2d(float(_smushWidth)/ float(_gameWidth), float(_smushHeight) / float(_gameHeight)));
	_smushProgram->setUniform("offset", Math::Vector2d(float(offsetX) / float(_gameWidth), float(offsetY) / float(_gameHeight)));
	metalBindTexture(kTexture2D, _smushTexId);

	metalDrawElements(kTriangles, 6, kUnsignedShort, nullptr);

	metalBindBuffer(kElementArrayBuffer, 0);
	metalEnable(kDepthTest);
}

void GfxMetal::releaseMovieFrame() {
	if (_smushTexId > 0) {
		metalDeleteTextures(1, &_smushTexId);
		_smushTexId = 0;
	}
}


const char *GfxMetal::getVideoDeviceName() {
	return "Apple Silicon Metal 4";
}

void GfxMetal::renderBitmaps(bool render) {
}

void GfxMetal::renderZBitmaps(bool render) {
}

void GfxMetal::createEMIModel(EMIModel *model) {
	EMIModelUserData *mud = new EMIModelUserData;
	model->_userData = mud;
	mud->_verticesVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, model->_numVertices * 3 * sizeof(float), model->_vertices, kStreamDraw);

	mud->_normalsVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, model->_numVertices * 3 * sizeof(float), model->_normals, kStreamDraw);

	mud->_texCoordsVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, model->_numVertices * 2 * sizeof(float), model->_texVerts, kStaticDraw);

	mud->_colorMapVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, model->_numVertices * 4 * sizeof(byte), model->_colorMap, kStaticDraw);

	MetalBackend::Shader * actorShader = _actorProgram->clone();
	actorShader->enableVertexAttribute("position", mud->_verticesVBO, 3, kFloat, kFalse, 3 * sizeof(float), 0);
	actorShader->enableVertexAttribute("normal", mud->_normalsVBO, 3, kFloat, kFalse, 3 * sizeof(float), 0);
	actorShader->enableVertexAttribute("texcoord", mud->_texCoordsVBO, 2, kFloat, kFalse, 2 * sizeof(float), 0);
	actorShader->enableVertexAttribute("color", mud->_colorMapVBO, 4, kUnsignedByte, kTrue, 4 * sizeof(byte), 0);
	mud->_shader = actorShader;

	actorShader = _actorLightsProgram->clone();
	actorShader->enableVertexAttribute("position", mud->_verticesVBO, 3, kFloat, kFalse, 3 * sizeof(float), 0);
	actorShader->enableVertexAttribute("normal", mud->_normalsVBO, 3, kFloat, kFalse, 3 * sizeof(float), 0);
	actorShader->enableVertexAttribute("texcoord", mud->_texCoordsVBO, 2, kFloat, kFalse, 2 * sizeof(float), 0);
	actorShader->enableVertexAttribute("color", mud->_colorMapVBO, 4, kUnsignedByte, kTrue, 4 * sizeof(byte), 0);
	mud->_shaderLights = actorShader;

	for (uint32 i = 0; i < model->_numFaces; ++i) {
		EMIMeshFace * face = &model->_faces[i];
		face->_indicesEBO = MetalBackend::Shader::createBuffer(kElementArrayBuffer, face->_faceLength * 3 * sizeof(uint16), face->_indexes, kStaticDraw);
	}

	metalBindBuffer(kElementArrayBuffer, 0);
}

void GfxMetal::destroyEMIModel(EMIModel *model) {
	for (uint32 i = 0; i < model->_numFaces; ++i) {
		EMIMeshFace *face = &model->_faces[i];
		MetalBackend::Shader::freeBuffer(face->_indicesEBO);
		face->_indicesEBO = 0;
	}

	EMIModelUserData *mud = static_cast<EMIModelUserData *>(model->_userData);

	if (mud) {
		MetalBackend::Shader::freeBuffer(mud->_verticesVBO);
		MetalBackend::Shader::freeBuffer(mud->_normalsVBO);
		MetalBackend::Shader::freeBuffer(mud->_texCoordsVBO);
		MetalBackend::Shader::freeBuffer(mud->_colorMapVBO);

		delete mud->_shader;
		delete mud->_shaderLights;
		delete mud;
	}

	model->_userData = nullptr;
}

void GfxMetal::createMesh(Mesh *mesh) {
	Common::Array<GrimVertex> meshInfo;
	meshInfo.reserve(mesh->_numVertices * 5);
	for (int i = 0; i < mesh->_numFaces; ++i) {
		MeshFace *face = &mesh->_faces[i];
		face->_userData = new uint32;
		*(uint32 *)face->_userData = meshInfo.size();

		if (face->getNumVertices() < 3)
			continue;

#define VERT(j) (&mesh->_vertices[3 * face->getVertex(j)])
#define TEXVERT(j) (face->hasTexture() ? &mesh->_textureVerts[2 * face->getTextureVertex(j)] : zero_texVerts)
#define NORMAL(j) (&mesh->_vertNormals[3 * face->getVertex(j)])

		for (int j = 2; j < face->getNumVertices(); ++j) {
			meshInfo.push_back(GrimVertex(VERT(0), TEXVERT(0), NORMAL(0)));
			meshInfo.push_back(GrimVertex(VERT(j - 1), TEXVERT(j - 1), NORMAL(j - 1)));
			meshInfo.push_back(GrimVertex(VERT(j), TEXVERT(j), NORMAL(j)));
		}

#undef VERT
#undef TEXVERT
#undef NORMAL

	}

	if (meshInfo.empty()) {
		mesh->_userData = nullptr;
		return;
	}

	ModelUserData *mud = new ModelUserData;
	mesh->_userData = mud;

	mud->_meshInfoVBO = MetalBackend::Shader::createBuffer(kArrayBuffer, meshInfo.size() * sizeof(GrimVertex), &meshInfo[0], kStaticDraw);

	MetalBackend::Shader *actorShader = _actorProgram->clone();
	actorShader->enableVertexAttribute("position", mud->_meshInfoVBO, 3, kFloat, kFalse, sizeof(GrimVertex), 0);
	actorShader->enableVertexAttribute("texcoord", mud->_meshInfoVBO, 2, kFloat, kFalse, sizeof(GrimVertex), 3 * sizeof(float));
	actorShader->enableVertexAttribute("normal", mud->_meshInfoVBO, 3, kFloat, kFalse, sizeof(GrimVertex), 5 * sizeof(float));
	actorShader->disableVertexAttribute("color", Math::Vector4d(1.f, 1.f, 1.f, 1.f));
	mud->_shader = actorShader;

	actorShader = _actorLightsProgram->clone();
	actorShader->enableVertexAttribute("position", mud->_meshInfoVBO, 3, kFloat, kFalse, sizeof(GrimVertex), 0);
	actorShader->enableVertexAttribute("texcoord", mud->_meshInfoVBO, 2, kFloat, kFalse, sizeof(GrimVertex), 3 * sizeof(float));
	actorShader->enableVertexAttribute("normal", mud->_meshInfoVBO, 3, kFloat, kFalse, sizeof(GrimVertex), 5 * sizeof(float));
	actorShader->disableVertexAttribute("color", Math::Vector4d(1.f, 1.f, 1.f, 1.f));
	mud->_shaderLights = actorShader;
}

void GfxMetal::destroyMesh(const Mesh *mesh) {
	ModelUserData *mud = static_cast<ModelUserData *>(mesh->_userData);

	for (int i = 0; i < mesh->_numFaces; ++i) {
		MeshFace *face = &mesh->_faces[i];
		if (face->_userData) {
			uint32 *data = static_cast<uint32 *>(face->_userData);
			delete data;
		}
	}

	if (!mud)
		return;

	delete mud->_shader;
	delete mud->_shaderLights;
	MetalBackend::Shader::freeBuffer(mud->_meshInfoVBO);
	delete mud;
}

Bitmap *GfxMetal::getScreenshot(int w, int h, bool useStored) {
	Graphics::Surface src;
	src.create(_screenWidth, _screenHeight, Graphics::PixelFormat::createFormatRGBA32());
	Bitmap *bmp;

	metalCapture(_storedDisplay, useStored, src);
	bmp = createScreenshotBitmap(&src, w, h, false);
	src.free();
	return bmp;
}

void GfxMetal::createSpecialtyTextureFromScreen(uint id, uint8 *data, int x, int y, int width, int height) {
	g_system->presentBuffer();
	metalCaptureRect(x, y, width, height, data);
	createSpecialtyTexture(id, data, width, height);
}

void GfxMetal::setBlendMode(bool additive) {
	if (additive) {
		metalBlendFunc(kSrcAlpha, kOne);
	} else {
		metalBlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
	}
}

}

#endif
