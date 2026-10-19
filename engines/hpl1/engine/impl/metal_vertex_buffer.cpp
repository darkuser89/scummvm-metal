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
 * Copyright (C) 2006-2010 - Frictional Games
 *
 * This file is part of HPL1 Engine.
 */

#include "hpl1/engine/impl/metal_vertex_buffer.h"
#include "hpl1/engine/impl/low_level_graphics_metal.h"
#include "hpl1/debug.h"
#include "hpl1/engine/math/Math.h"
#include "hpl1/engine/system/low_level_system.h"



namespace hpl {

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

MetalVertexBuffer::MetalVertexBuffer(iLowLevelGraphics *apLowLevelGraphics, tVertexFlag aFlags,
								   eVertexBufferDrawType aDrawType, eVertexBufferUsageType aUsageType,
								   int alReserveVtxSize, int alReserveIdxSize) : iVertexBuffer(apLowLevelGraphics, aFlags, aDrawType, aUsageType, alReserveVtxSize, alReserveIdxSize) {



	if (alReserveVtxSize > 0) {
		for (int i = 0; i < klNumOfVertexFlags; i++) {
			if (aFlags & kvVertexFlags[i]) {
				mvVertexArray[i].reserve(alReserveVtxSize * kvVertexElements[i]);
			}
		}
	}

	if (alReserveIdxSize > 0)
		mvIndexArray.reserve(alReserveIdxSize);

	mbTangents = false;

	mbHasShadowDouble = false;

	mpLowLevelGraphics = apLowLevelGraphics;
}

MetalVertexBuffer::~MetalVertexBuffer() {
	delete _vertices; delete _indices;
	for (int i = 0; i < klNumOfVertexFlags; i++)
		mvVertexArray[i].clear();

	mvIndexArray.clear();
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void MetalVertexBuffer::AddVertex(tVertexFlag aType, const cVector3f &avVtx) {
	_verticesDirty = true;
	int idx = cMath::Log2ToInt((int)aType);

	mvVertexArray[idx].push_back(avVtx.x);
	mvVertexArray[idx].push_back(avVtx.y);
	mvVertexArray[idx].push_back(avVtx.z);
	if (kvVertexElements[idx] == 4)
		mvVertexArray[idx].push_back(1);
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::AddColor(tVertexFlag aType, const cColor &aColor) {
	_verticesDirty = true;
	int idx = cMath::Log2ToInt((int)aType);

	mvVertexArray[idx].push_back(aColor.r);
	mvVertexArray[idx].push_back(aColor.g);
	mvVertexArray[idx].push_back(aColor.b);
	mvVertexArray[idx].push_back(aColor.a);
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::AddIndex(unsigned int alIndex) {
	_indicesDirty = true;
	mvIndexArray.push_back(alIndex);
}

//-----------------------------------------------------------------------

bool MetalVertexBuffer::Compile(tVertexCompileFlag aFlags) {
	_verticesDirty = _indicesDirty = true;
	if (aFlags & eVertexCompileFlag_CreateTangents) {
		int vertexCount = GetVertexNum();
		if (vertexCount <= 0 || GetIndexNum() < 3 || GetIndexNum() % 3 ||
		    mvVertexArray[0].size() < uint(vertexCount) * 3 || mvVertexArray[4].size() < uint(vertexCount) * 3)
			return false;
		for (uint index : mvIndexArray) if (index >= uint(vertexCount)) return false;

		int idx = cMath::Log2ToInt((int)eVertexFlag_Texture1);

		int lSize = GetVertexNum() * 4;
		mvVertexArray[idx].resize(lSize);

		if (!cMath::CreateTriTangentVectors(&(mvVertexArray[cMath::Log2ToInt((int)eVertexFlag_Texture1)][0]),
									   &mvIndexArray[0], GetIndexNum(),

									   &(mvVertexArray[cMath::Log2ToInt((int)eVertexFlag_Position)][0]),
									   kvVertexElements[cMath::Log2ToInt((int)eVertexFlag_Position)],

									   &(mvVertexArray[cMath::Log2ToInt((int)eVertexFlag_Texture0)][0]),
									   &(mvVertexArray[cMath::Log2ToInt((int)eVertexFlag_Normal)][0]),
										   GetVertexNum())) return false;
		mbTangents = true;
		mVertexFlags |= eVertexFlag_Texture1;
	}

	return true;
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::UpdateData(tVertexFlag aTypes, bool abIndices) {
	_verticesDirty |= aTypes != 0;
	_indicesDirty |= abIndices;
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::CreateShadowDouble(bool abUpdateData) {
	_verticesDirty = true;
	int lIdx = cMath::Log2ToInt(eVertexFlag_Position);

	// Set to new size.
	int lSize = (int)mvVertexArray[lIdx].size();
	mvVertexArray[lIdx].reserve(lSize * 2);

	int lCount = lSize / 4;
	for (int i = 0; i < lCount; i++) {
		mvVertexArray[lIdx].push_back(mvVertexArray[lIdx][i * 4 + 0]);
		mvVertexArray[lIdx].push_back(mvVertexArray[lIdx][i * 4 + 1]);
		mvVertexArray[lIdx].push_back(mvVertexArray[lIdx][i * 4 + 2]);
		mvVertexArray[lIdx].push_back(0); // 0);
	}

	mbHasShadowDouble = true;

	if (abUpdateData) {
		UpdateData(eVertexFlag_Position, false);
	}
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::Transform(const cMatrixf &a_mtxTransform) {
	float *pPosArray = GetArray(eVertexFlag_Position);
	float *pNormalArray = GetArray(eVertexFlag_Normal);
	float *pTangentArray = NULL;
	if (mbTangents)
		pTangentArray = GetArray(eVertexFlag_Texture1);

	int lVtxNum = GetVertexNum();

	cMatrixf mtxRot = a_mtxTransform.GetRotation();

	int lVtxStride = kvVertexElements[cMath::Log2ToInt(eVertexFlag_Position)];

	int lOffset = GetVertexNum() * 4;

	for (int i = 0; i < lVtxNum; i++) {
		float *pPos = &pPosArray[i * lVtxStride];
		float *pNorm = pNormalArray ? &pNormalArray[i * 3] : nullptr;
		float *pTan = NULL;
		if (mbTangents)
			pTan = &pTangentArray[i * 4];

		cVector3f vPos = cMath::MatrixMul(a_mtxTransform, cVector3f(pPos[0], pPos[1], pPos[2]));
		pPos[0] = vPos.x;
		pPos[1] = vPos.y;
		pPos[2] = vPos.z;

		if (mbHasShadowDouble) {
			float *pExtraPos = &pPosArray[i * lVtxStride + lOffset];
			pExtraPos[0] = vPos.x;
			pExtraPos[1] = vPos.y;
			pExtraPos[2] = vPos.z;
		}

		if (pNorm) {
			cVector3f vNorm = cMath::MatrixMul(mtxRot, cVector3f(pNorm[0], pNorm[1], pNorm[2]));
			vNorm.Normalise();
			pNorm[0] = vNorm.x;
			pNorm[1] = vNorm.y;
			pNorm[2] = vNorm.z;
		}

		if (mbTangents) {
			cVector3f vTan = cMath::MatrixMul(mtxRot, cVector3f(pTan[0], pTan[1], pTan[2]));
			vTan.Normalise();
			pTan[0] = vTan.x;
			pTan[1] = vTan.y;
			pTan[2] = vTan.z;
		}
	}

	if (mbTangents)
		UpdateData(eVertexFlag_Position | eVertexFlag_Normal | eVertexFlag_Texture1, false);
	else
		UpdateData(eVertexFlag_Position | eVertexFlag_Normal, false);
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::Draw(eVertexBufferDrawType aDrawType) {
	eVertexBufferDrawType type = aDrawType == eVertexBufferDrawType_LastEnum ? mDrawType : aDrawType;
	DrawIndices(mvIndexArray.data(), mlElementNum < 0 ? mvIndexArray.size() : mlElementNum, type);
}

void MetalVertexBuffer::DrawIndices(unsigned int *apIndices, int alCount,
								   eVertexBufferDrawType aDrawType) {
	if (!apIndices || alCount <= 0) return;
	uint count = mvVertexArray[1].size() / 4;
	if (_vertices && _vertices->size() / sizeof(MetalShaderVertex) != count) _indicesDirty = true;
	Common::Array<MetalShaderVertex> vertices;
	if (_verticesDirty || !_vertices) {
		vertices.resize(count);
		for (uint i = 0; i < count; ++i) {
			MetalShaderVertex &vertex = vertices[i];
			memset(&vertex, 0, sizeof(vertex));
			memcpy(vertex.position, mvVertexArray[1].data() + i * 4, 4 * sizeof(float));
			vertex.normal[2] = 1;
			for (uint unit = 0; unit < 8; ++unit) vertex.texcoord[unit][3] = 1;
			for (float &channel : vertex.color) channel = 1;
			for (uint attribute = 0; attribute < klNumOfVertexFlags; ++attribute) {
				if (!(mVertexFlags & kvVertexFlags[attribute]) || attribute == 1 || attribute == 3) continue;
				uint components = mbTangents && attribute == 5 ? 4 : kvVertexElements[attribute];
				if ((i + 1) * components > mvVertexArray[attribute].size()) continue;
				float *destination = attribute == 0 ? vertex.normal : attribute == 2 ? vertex.color : vertex.texcoord[attribute - 4];
				memcpy(destination, mvVertexArray[attribute].data() + i * components, components * sizeof(float));
			}
		}
		Graphics::Metal::Renderer &renderer = static_cast<LowLevelGraphicsMetal *>(mpLowLevelGraphics)->metal();
		if (!count) return;
		if (_vertices) renderer.updateBuffer(*_vertices, vertices.data(), vertices.size() * sizeof(MetalShaderVertex));
		else _vertices = renderer.createBuffer(vertices.data(), vertices.size() * sizeof(MetalShaderVertex));
		if (!_vertices) error("HPL1: could not allocate persistent vertices");
		_verticesDirty = false;
	}
	eVertexBufferDrawType type = aDrawType == eVertexBufferDrawType_LastEnum ? mDrawType : aDrawType;
	Graphics::Metal::GeometryBinding geometry; geometry.vertices = _vertices;
	if (apIndices == mvIndexArray.data() && type != eVertexBufferDrawType_Quad) {
		Graphics::Metal::Renderer &renderer = static_cast<LowLevelGraphicsMetal *>(mpLowLevelGraphics)->metal();
		if (_indicesDirty || !_indices) {
			for (uint index : mvIndexArray) if (index >= count) error("HPL1: persistent vertex index out of range");
			if (_indices) renderer.updateBuffer(*_indices, mvIndexArray.data(), mvIndexArray.size() * sizeof(uint32));
			else _indices = renderer.createBuffer(mvIndexArray.data(), mvIndexArray.size() * sizeof(uint32));
			if (!_indices) error("HPL1: could not allocate persistent indices");
			_indicesDirty = false;
		}
		geometry.indices = _indices;
	}
	static_cast<LowLevelGraphicsMetal *>(mpLowLevelGraphics)->drawVertices(nullptr, count, apIndices, alCount, type, &geometry);
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::Bind() {

}

//-----------------------------------------------------------------------

void MetalVertexBuffer::UnBind() {

}

//-----------------------------------------------------------------------

iVertexBuffer *MetalVertexBuffer::CreateCopy(eVertexBufferUsageType aUsageType) {
	MetalVertexBuffer *pVtxBuff = hplNew(MetalVertexBuffer, (mpLowLevelGraphics,
														   mVertexFlags, mDrawType, aUsageType,
														   GetVertexNum(), GetIndexNum()));

	// Copy the vertices to the new buffer.
	for (int i = 0; i < klNumOfVertexFlags; i++) {
		if (kvVertexFlags[i] & mVertexFlags) {
#if 0
			int lElements = kvVertexElements[i];
			if (mbTangents && kvVertexFlags[i] == eVertexFlag_Texture1)
				lElements = 4;
#endif

			pVtxBuff->ResizeArray(kvVertexFlags[i], (int)mvVertexArray[i].size());

			if (!mvVertexArray[i].empty())
				memcpy(pVtxBuff->GetArray(kvVertexFlags[i]), mvVertexArray[i].data(), mvVertexArray[i].size() * sizeof(float));
		}
	}

	// Copy indices to the new buffer
	pVtxBuff->ResizeIndices(GetIndexNum());
	if (GetIndexNum()) memcpy(pVtxBuff->GetIndices(), GetIndices(), GetIndexNum() * sizeof(unsigned int));

	pVtxBuff->mbTangents = mbTangents;
	pVtxBuff->mbHasShadowDouble = mbHasShadowDouble;

	pVtxBuff->Compile(0);

	return pVtxBuff;
}

//-----------------------------------------------------------------------

cBoundingVolume MetalVertexBuffer::CreateBoundingVolume() {
	cBoundingVolume bv;

	int lNum = cMath::Log2ToInt((int)eVertexFlag_Position);
	if (mvVertexArray[lNum].empty()) return bv;
	bv.AddArrayPoints(&(mvVertexArray[lNum][0]), GetVertexNum());
	bv.CreateFromPoints(kvVertexElements[cMath::Log2ToInt(eVertexFlag_Position)]);

	return bv;
}

float *MetalVertexBuffer::GetArray(tVertexFlag aType) {
	_verticesDirty = true;
	int idx = cMath::Log2ToInt((int)aType);

	return mvVertexArray[idx].empty() ? nullptr : mvVertexArray[idx].data();
}

//-----------------------------------------------------------------------

unsigned int *MetalVertexBuffer::GetIndices() {
	_indicesDirty = true;
	return mvIndexArray.empty() ? nullptr : mvIndexArray.data();
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::ResizeArray(tVertexFlag aType, int alSize) {
	_verticesDirty = true;
	int idx = cMath::Log2ToInt((int)aType);

	mvVertexArray[idx].resize(alSize);
}

//-----------------------------------------------------------------------

void MetalVertexBuffer::ResizeIndices(int alSize) {
	_indicesDirty = true;
	mvIndexArray.resize(alSize);
}

//-----------------------------------------------------------------------

int MetalVertexBuffer::GetVertexNum() {
	int idx = cMath::Log2ToInt((int)eVertexFlag_Position);
	int lSize = (int)mvVertexArray[idx].size() / kvVertexElements[idx];

	// If there is a shadow double, just return the length of the first half.
	if (mbHasShadowDouble)
		return lSize / 2;
	else
		return lSize;
}

int MetalVertexBuffer::GetIndexNum() {
	return (int)mvIndexArray.size();
}

cVector3f MetalVertexBuffer::GetVector3(tVertexFlag aType, unsigned alIdx) {
	if (!(aType & mVertexFlags))
		return cVector3f(0, 0, 0);

	int idx = cMath::Log2ToInt((int)aType);
	int pos = alIdx * kvVertexElements[idx];

	return cVector3f(mvVertexArray[idx][pos + 0], mvVertexArray[idx][pos + 1],
					 mvVertexArray[idx][pos + 2]);
}
cVector3f MetalVertexBuffer::GetVector4(tVertexFlag aType, unsigned alIdx) {
	if (!(aType & mVertexFlags))
		return cVector3f(0, 0, 0);

	int idx = cMath::Log2ToInt((int)aType);
	int pos = alIdx * 4; // kvVertexElements[idx];

	return cVector3f(mvVertexArray[idx][pos + 0], mvVertexArray[idx][pos + 1],
					 mvVertexArray[idx][pos + 2]);
}
cColor MetalVertexBuffer::GetColor(tVertexFlag aType, unsigned alIdx) {
	if (!(aType & mVertexFlags))
		return cColor();

	int idx = cMath::Log2ToInt((int)aType);
	int pos = alIdx * kvVertexElements[idx];

	return cColor(mvVertexArray[idx][pos + 0], mvVertexArray[idx][pos + 1],
				  mvVertexArray[idx][pos + 2], mvVertexArray[idx][pos + 3]);
}
unsigned int MetalVertexBuffer::GetIndex(tVertexFlag aType, unsigned alIdx) {
	return mvIndexArray[alIdx];
}

//-----------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
/////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

} // namespace hpl
