/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_RETRO_PIPELINE_H
#define GRAPHICS_METAL_RETRO_PIPELINE_H

#include "graphics/metal/retro/compiler.h"

namespace Graphics { namespace Metal { namespace Retro {

class Pipeline : public Common::NonCopyable {
public:
	explicit Pipeline(Renderer &renderer) : _renderer(renderer) {}
	~Pipeline() { close(); }
	bool open(const Common::Path &path, Common::SearchSet &archives);
	void close();
	bool isOpen() const { return _preset != nullptr; }
	bool isAnimated() const { return _animated; }
	// Input and result have the backend's native top-left texture orientation.
	Texture *render(Texture &input, uint viewportWidth, uint viewportHeight, bool linear);
	const Common::String &error() const { return _error; }
private:
	struct TextureSource {
		enum Kind { kInput, kHistory, kPass, kLookup };
		Kind kind;
		uint index;
		TextureSource(Kind k = kInput, uint i = 0) : kind(k), index(i) {}
	};
	struct Pass {
		CompiledShader *shader = nullptr;
		RenderTarget *target = nullptr;
		Common::Array<TextureSource> samplers;
		Common::Array<TextureBinding> bindings;
		Common::Array<byte> vertices;
		Buffer *geometry = nullptr;
	};
	Renderer &_renderer;
	ShaderPreset *_preset = nullptr;
	Common::Array<Pass> _passes;
	Common::Array<Texture *> _lookups;
	RenderTarget *_history[8] = {};
	RenderTarget *_output = nullptr;
	uint _current = 0, _frame = 0;
	uint _inputWidth = 0, _inputHeight = 0, _viewportWidth = 0, _viewportHeight = 0;
	uint _historyCount = 1;
	bool _animated = false;
	UniformsMap _parameters;
	Common::String _error;
	bool ensureTargets(uint inputWidth, uint inputHeight, uint viewportWidth, uint viewportHeight);
	TextureSource resolveTexture(const Common::String &name, uint pass) const;
	Texture *findTexture(const TextureSource &source, Texture &input);
	void setTextureSizes(CompiledShader &shader, const Common::String &prefix, Texture &texture);
};

} } } // namespace Graphics::Metal::Retro

#endif
