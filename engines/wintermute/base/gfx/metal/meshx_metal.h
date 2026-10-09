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

#ifndef WINTERMUTE_XMESH_METAL_SHADER_H
#define WINTERMUTE_XMESH_METAL_SHADER_H

#include "engines/wintermute/base/gfx/xmesh.h"

class Effect3D;
class Effect3DParams;

#if defined(USE_METAL)

#include "engines/wintermute/base/gfx/metal/metal_draw.h"

namespace Wintermute {

class XMeshMetal : public XMesh {
public:
	XMeshMetal(BaseGame *inGame, MetalBackend::Shader *shader, MetalBackend::Shader *flatShadowShader);
	~XMeshMetal() override;

	bool loadFromXData(const char *filename, XFileData *xobj) override;
	bool render(XModel *model) override;
	bool renderFlatShadowModel(uint32 shadowColor) override;
	bool update(FrameNode *parentFrame) override;

private:
	bool setupEffect(Material *material, bool secondPassNeeded);

protected:
	uint32 _vertexBuffer;
	uint32 _indexBuffer;

	MetalBackend::Shader *_shader;
	MetalBackend::Shader *_flatShadowShader;
};

} // namespace Wintermute

#endif // defined(USE_METAL)

#endif
