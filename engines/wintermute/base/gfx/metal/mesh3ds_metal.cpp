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

#include "engines/wintermute/wintypes.h"


#if defined(USE_METAL)

#include "engines/wintermute/base/gfx/metal/mesh3ds_metal.h"

namespace Wintermute {

using namespace MetalBackend;

Mesh3DSMetal::Mesh3DSMetal(BaseGame *inGame, MetalBackend::Shader *shader) : Mesh3DS(inGame), _shader(shader) {
	_vertexCount = 0;
	_vertexData = nullptr;

	metalGenBuffers(1, &_vertexBuffer);
}

Mesh3DSMetal::~Mesh3DSMetal() {
	metalDeleteBuffers(1, &_vertexBuffer);
}

void Mesh3DSMetal::fillVertexBuffer() {
	_vertexCount = _numFaces * 3;
	_vertexData = (Mesh3DSVertex *)_vb.ptr();

	metalBindBuffer(kArrayBuffer, _vertexBuffer);
	metalBufferData(kArrayBuffer, sizeof(Mesh3DSVertex) * _vertexCount, _vertexData, kStaticDraw);
	metalBindBuffer(kArrayBuffer, 0);
}

void Mesh3DSMetal::render(bool color) {
	if (_vertexCount == 0)
		return;

	_shader->enableVertexAttribute("position", _vertexBuffer, 3, kFloat, false, sizeof(Mesh3DSVertex), 0);
	if (color)
		_shader->enableVertexAttribute("color", _vertexBuffer, 4, kFloat, false, sizeof(Mesh3DSVertex), 24);
	_shader->use(true);

	metalDrawArrays(kTriangles, 0, _vertexCount);
}

} // namespace Wintermute

#endif // defined(USE_METAL)
