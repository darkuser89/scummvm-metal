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
	// Input and result have the backend's native top-left texture orientation.
	Texture *render(Texture &input, uint viewportWidth, uint viewportHeight, bool linear);
	const Common::String &error() const { return _error; }
private:
	struct Pass {
		CompiledShader *shader = nullptr;
		RenderTarget *target = nullptr;
	};
	Renderer &_renderer;
	ShaderPreset *_preset = nullptr;
	Common::Array<Pass> _passes;
	Common::Array<Texture *> _lookups;
	RenderTarget *_history[8] = {};
	RenderTarget *_output = nullptr;
	uint _current = 0, _frame = 0;
	UniformsMap _parameters;
	Common::String _error;
	bool ensureTargets(uint inputWidth, uint inputHeight, uint viewportWidth, uint viewportHeight);
	Texture *findTexture(const Common::String &name, uint pass, Texture &input, bool &lookup, uint &lookupIndex);
	void setTextureSizes(CompiledShader &shader, const Common::String &prefix, Texture &texture);
};

} } } // namespace Graphics::Metal::Retro

#endif
