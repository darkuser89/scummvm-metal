/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_RETRO_COMPILER_H
#define GRAPHICS_METAL_RETRO_COMPILER_H

#include "graphics/metal/retro/types.h"

namespace Graphics { namespace Metal { namespace Retro {

void readParameterDefaults(const Common::String &source, UniformsMap &parameters);

class SourceReader {
public:
	SourceReader(Common::Archive *container, Common::SearchSet &archives, UniformsMap *parameters = nullptr) :
		_container(container), _archives(archives), _parameters(parameters) {}
	Common::SeekableReadStream *open(const Common::Path &path) const;
	bool read(const Common::Path &path, Common::String &text) const;
private:
	Common::Archive *_container;
	Common::SearchSet &_archives;
	UniformsMap *_parameters;
};

struct ShaderAttribute {
	Common::String name;
	VertexAttribute layout;
	bool generic = false;
};

struct ShaderUniform {
	enum Type { kFloat, kInt, kUInt, kBool };
	Common::String name;
	uint offset = 0, components = 1, columns = 1, matrixStride = 0;
	Type type = kFloat;
	bool rowMajor = false, fragment = false;
};

class CompiledShader : public Common::NonCopyable {
public:
	~CompiledShader() { delete program; }
	Program *program = nullptr;
	Common::Array<ShaderAttribute> attributes;
	Common::StringArray samplers;
	Common::Array<ShaderUniform> uniforms;
	Common::Array<byte> vertexData, fragmentData;
	uint vertexStride = 0;
	void setUniform(const Common::String &name, const float *values, uint count);
	void setUniform(const Common::String &name, float value) { setUniform(name, &value, 1); }
	void setUniformInt(const Common::String &name, uint32 value) { setUniformValues(name, nullptr, &value, 1); }
	bool hasUniform(const Common::String &name) const;
private:
	void setUniformValues(const Common::String &name, const float *values, const uint32 *integers, uint count);
};

class ShaderCompiler {
public:
	ShaderCompiler(Renderer &renderer, const SourceReader &reader) : _renderer(renderer), _reader(reader) {}
	CompiledShader *compile(const Common::Path &path, const Common::String &source, const Common::String &aliases);
	const Common::String &error() const { return _error; }
private:
	Renderer &_renderer;
	const SourceReader &_reader;
	Common::String _error;
};

} } } // namespace Graphics::Metal::Retro

#endif
