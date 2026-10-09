/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/metal/retro/pipeline.h"
#include "graphics/metal/retro/parser.h"
#include "common/stream.h"
#include "common/textconsole.h"
#include "image/bmp.h"
#include "image/jpeg.h"
#include "image/png.h"
#include "image/tga.h"
#include <cmath>

namespace Graphics { namespace Metal { namespace Retro {

namespace {

template<class Decoder> static Texture *loadImage(Renderer &renderer, Common::SeekableReadStream &stream) {
	Decoder decoder;
	if (!decoder.loadStream(stream) || !decoder.getSurface()) return nullptr;
	const Graphics::Palette &palette = static_cast<Image::ImageDecoder &>(decoder).getPalette();
	Surface *rgba = decoder.getSurface()->convertTo(PixelFormat::createFormatRGBA32(), palette.data(), palette.size());
	if (!rgba) return nullptr;
	Texture *texture = renderer.createTexture(*rgba);
	rgba->free(); delete rgba;
	return texture;
}

static Texture *loadLookup(Renderer &renderer, const SourceReader &reader, const Common::Path &path) {
	Common::SeekableReadStream *stream = reader.open(path);
	if (!stream) return nullptr;
	Common::String name = path.baseName(); name.toLowercase();
	Texture *texture = nullptr;
	if (name.hasSuffix(".png")) texture = loadImage<Image::PNGDecoder>(renderer, *stream);
	else if (name.hasSuffix(".jpg") || name.hasSuffix(".jpeg")) texture = loadImage<Image::JPEGDecoder>(renderer, *stream);
	else if (name.hasSuffix(".bmp")) texture = loadImage<Image::BitmapDecoder>(renderer, *stream);
	else if (name.hasSuffix(".tga")) texture = loadImage<Image::TGADecoder>(renderer, *stream);
	delete stream;
	return texture;
}

static bool numbered(const Common::String &name, const char *prefix, uint &number) {
	if (!name.hasPrefix(prefix)) return false;
	const char *digits = name.c_str() + strlen(prefix);
	if (!*digits) return false;
	number = 0;
	while (*digits) {
		if (*digits < '0' || *digits > '9' || number > 100000) return false;
		number = number * 10 + *digits++ - '0';
	}
	return true;
}

static bool textureCoordinate(const Common::String &name, uint pass, const ShaderPreset &preset) {
	if (name == "TexCoord" || name == "OrigTexCoord" || name == "PrevTexCoord" ||
	    (name == "LUTTexCoord" && !preset.textures.empty())) return true;
	if (!name.hasSuffix("TexCoord")) return false;
	Common::String prefix = name; prefix.erase(prefix.size() - 8);
	uint index = 0;
	if (numbered(prefix, "Prev", index) && index >= 1 && index <= 6) return true;
	if (pass && numbered(prefix, "PassPrev", index) && index >= 1 && index <= pass + 1) return true;
	if (numbered(prefix, "Pass", index) && index >= 1 && index <= pass) return true;
	for (uint prior = 0; prior < pass; ++prior)
		if (!preset.passes[prior].alias.empty() && prefix == preset.passes[prior].alias) return true;
	return false;
}

} // anonymous namespace

bool Pipeline::open(const Common::Path &path, Common::SearchSet &archives) {
	close(); _error.clear();
	_preset = parsePreset(path, archives);
	if (!_preset || _preset->passes.empty()) { _error = "Preset contains no shader passes"; close(); return false; }
	SourceReader reader(_preset->container, archives, &_parameters);
	for (const auto &lookup : _preset->textures) {
		Texture *texture = loadLookup(_renderer, reader, _preset->basePath.join(lookup.fileName).normalize());
		if (!texture) { _error = "Could not load preset texture " + lookup.fileName; close(); return false; }
		_lookups.push_back(texture);
	}
	Common::String aliases;
	for (const auto &pass : _preset->passes)
		if (!pass.alias.empty()) aliases += "#define " + pass.alias + "_ALIAS\n";
	Common::Array<Common::String> sources;
	for (const auto &pass : _preset->passes) {
		Common::String source;
		if (!reader.read(_preset->basePath.join(pass.fileName).normalize(), source)) {
			_error = "Could not read preset shader " + pass.fileName; close(); return false;
		}
		sources.push_back(source);
	}
	ShaderCompiler compiler(_renderer, reader);
	for (uint i = 0; i < sources.size(); ++i) {
		Pass pass;
		pass.shader = compiler.compile(_preset->basePath.join(_preset->passes[i].fileName).normalize(), sources[i], aliases);
		if (!pass.shader) { _error = compiler.error(); close(); return false; }
		const ShaderPass &definition = _preset->passes[i];
		TargetFormat format = definition.floatFBO ? kTargetRGBA16Float : definition.srgbFBO ? kTargetSRGBA8 : kTargetBGRA8;
		if (!_renderer.validateProgram(*pass.shader->program, format)) {
			delete pass.shader; _error = "Metal rejected the preset stage interface"; close(); return false;
		}
		pass.shader->setUniformInt("FrameDirection", 1);
		_passes.push_back(pass);
	}
	for (const auto &parameter : _preset->parameters) _parameters[parameter._key] = parameter._value;
	for (auto &pass : _passes)
		for (const auto &parameter : _parameters) pass.shader->setUniform(parameter._key, parameter._value);
	return true;
}

void Pipeline::close() {
	for (auto &pass : _passes) {
		if (_renderer.renderTarget() == pass.target) _renderer.setRenderTarget(nullptr);
		delete pass.shader; delete pass.target;
	}
	_passes.clear();
	for (auto *texture : _lookups) delete texture;
	_lookups.clear();
	for (auto &target : _history) {
		if (_renderer.renderTarget() == target) _renderer.setRenderTarget(nullptr);
		delete target; target = nullptr;
	}
	if (_renderer.renderTarget() == _output) _renderer.setRenderTarget(nullptr);
	delete _output; _output = nullptr;
	delete _preset; _preset = nullptr;
	_parameters.clear(); _frame = _current = 0;
}

bool Pipeline::ensureTargets(uint inputWidth, uint inputHeight, uint viewportWidth, uint viewportHeight) {
	if (!_preset || !inputWidth || !inputHeight || !viewportWidth || !viewportHeight) return false;
	bool reset = !_history[0] || _history[0]->texture().width() != inputWidth || _history[0]->texture().height() != inputHeight;
	if (reset) {
		for (auto &target : _history) {
			delete target;
			target = _renderer.createRenderTarget(inputWidth, inputHeight, 1, kTargetBGRA8, true);
			if (!target) { _error = "Could not allocate preset frame history"; return false; }
			_renderer.setRenderTarget(target); _renderer.clear(0, 0, 0, 0);
		}
		_frame = _current = 0;
	}
	float width = inputWidth, height = inputHeight;
	for (uint i = 0; i < _passes.size(); ++i) {
		const ShaderPass &definition = _preset->passes[i];
		definition.applyScale(width, height, viewportWidth, viewportHeight, &width, &height);
		if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 ||
		    width > _renderer.maxTextureSize() || height > _renderer.maxTextureSize()) {
			_error = "Preset pass requests an invalid render-target size"; return false;
		}
		Pass &pass = _passes[i];
		if (!pass.target || pass.target->texture().width() != uint(width) || pass.target->texture().height() != uint(height)) {
			delete pass.target;
			TargetFormat format = definition.floatFBO ? kTargetRGBA16Float : definition.srgbFBO ? kTargetSRGBA8 : kTargetBGRA8;
			pass.target = _renderer.createRenderTarget(uint(width), uint(height), 1, format, true);
			if (!pass.target) { _error = "Could not allocate preset pass target"; return false; }
		}
	}
	Texture &last = _passes.back().target->texture();
	if (!_output || _output->texture().width() != last.width() || _output->texture().height() != last.height()) {
		delete _output; _output = _renderer.createRenderTarget(last.width(), last.height());
	}
	if (!_output) { _error = "Could not allocate preset output texture"; return false; }
	return true;
}

Texture *Pipeline::findTexture(const Common::String &name, uint pass, Texture &input, bool &lookup, uint &lookupIndex) {
	lookup = false; lookupIndex = 0;
	Common::String identifier = name;
	uint bracket = identifier.findFirstOf('[');
	if (bracket != Common::String::npos) {
		// glUniform1i binds the first array element only; untouched
		// elements retain the sampler's default texture unit zero.
		for (uint pos = bracket; pos < identifier.size(); ++pos)
			if (identifier[pos] >= '1' && identifier[pos] <= '9') return &input;
		identifier.erase(bracket);
	}
	if (identifier == "Texture") return &input;
	for (uint i = 0; i < _preset->textures.size(); ++i) {
		if (identifier == _preset->textures[i].id) { lookup = true; lookupIndex = i; return _lookups[i]; }
	}
	Common::String prefix = identifier;
	if (prefix.hasSuffix("Texture")) prefix.erase(prefix.size() - 7);
	if (prefix == "Orig") return &_history[_current]->texture();
	uint index = 0;
	if (prefix == "Prev" || (numbered(prefix, "Prev", index) && index <= 6))
		return &_history[(_current + 8 - 1 - index) % 8]->texture();
	if (numbered(prefix, "PassPrev", index) && index > 0 && index <= pass + 1)
		return index == pass + 1 ? &_history[_current]->texture() : &_passes[pass - index].target->texture();
	if (numbered(prefix, "Pass", index) && index > 0 && index <= pass)
		return &_passes[index - 1].target->texture();
	for (uint i = 0; i < pass; ++i)
		if (!prefix.empty() && prefix == _preset->passes[i].alias) return &_passes[i].target->texture();
	// Unassigned samplers in the original GL program default to unit zero.
	return &input;
}

void Pipeline::setTextureSizes(CompiledShader &shader, const Common::String &prefix, Texture &texture) {
	const float size[] = {float(texture.width()), float(texture.height())};
	shader.setUniform(prefix + "InputSize", size, 2); shader.setUniform(prefix + "TextureSize", size, 2);
}

Texture *Pipeline::render(Texture &input, uint viewportWidth, uint viewportHeight, bool linear) {
	RenderTarget *previousTarget = _renderer.renderTarget();
	Common::Rect previousViewport = _renderer.viewport();
	if (!ensureTargets(input.width(), input.height(), viewportWidth, viewportHeight)) {
		_renderer.setRenderTarget(previousTarget); _renderer.setViewport(previousViewport); return nullptr;
	}
	// Row zero represents GL's lower edge. The GL input target maps the
	// game's top edge to that edge, so the input copy needs no reflection.
	// SPIRV-Cross flips clip-space Y to retain this storage convention.
	_renderer.copyTexture(_history[_current]->texture(), Common::Rect(input.width(), input.height()),
		input, Common::Rect(input.width(), input.height()));
	Texture *source = &_history[_current]->texture();
	for (uint i = 0; i < _passes.size(); ++i) {
		Pass &pass = _passes[i];
		CompiledShader &shader = *pass.shader;
		const ShaderPass &definition = _preset->passes[i];
		const uint width = pass.target->texture().width(), height = pass.target->texture().height();
		float outputSize[] = {float(width), float(height)};
		shader.setUniform("OutputSize", outputSize, 2);
		setTextureSizes(shader, "", *source); setTextureSizes(shader, "Orig", _history[_current]->texture());
		setTextureSizes(shader, "Prev", _history[(_current + 7) % 8]->texture());
		for (uint age = 1; age <= 6; ++age)
			setTextureSizes(shader, Common::String::format("Prev%u", age), _history[(_current + 7 - age) % 8]->texture());
		for (uint prior = 0; prior < i; ++prior) {
			Texture &texture = _passes[prior].target->texture();
			setTextureSizes(shader, Common::String::format("Pass%u", prior + 1), texture);
			setTextureSizes(shader, Common::String::format("PassPrev%u", i - prior), texture);
			if (!_preset->passes[prior].alias.empty()) setTextureSizes(shader, _preset->passes[prior].alias, texture);
		}
		setTextureSizes(shader, Common::String::format("PassPrev%u", i + 1), _history[_current]->texture());
		shader.setUniformInt("FrameCount", definition.frameCountMod ? _frame % definition.frameCountMod : _frame);
		const float matrix[] = {2.f / width, 0, 0, 0, 0, 2.f / height, 0, 0, 0, 0, 0, 0, -1, -1, 0, 1};
		shader.setUniform("MVPMatrix", matrix, 16);
		Common::Array<byte> vertices; vertices.resize(4 * shader.vertexStride); memset(vertices.begin(), 0, vertices.size());
		for (uint vertex = 0; vertex < 4; ++vertex) {
			for (const auto &attribute : shader.attributes) {
				float values[] = {0, 0, 0, 1};
				if (!attribute.generic && attribute.name == "VertexCoord") {
					values[0] = (vertex & 1) ? width : 0;
					bool bottom = vertex & 2;
					if (i + 1 == _passes.size()) bottom = !bottom;
					values[1] = bottom ? height : 0;
				} else if (!attribute.generic && textureCoordinate(attribute.name, i, *_preset)) {
					values[0] = (vertex & 1) ? 1 : 0; values[1] = (vertex & 2) ? 1 : 0;
				}
				memcpy(vertices.begin() + vertex * shader.vertexStride + attribute.layout.offset, values, attribute.layout.components * 4);
			}
		}
		Common::Array<TextureBinding> bindings; bindings.resize(shader.samplers.size());
		for (uint slot = 0; slot < bindings.size(); ++slot) {
			TextureBinding &binding = bindings[slot]; bool lookup; uint lookupIndex;
			binding.texture = findTexture(shader.samplers[slot], i, *source, lookup, lookupIndex);
			if (!binding.texture) {
				_error = "Preset requests an unavailable texture: " + shader.samplers[slot];
				_renderer.setRenderTarget(previousTarget); _renderer.setViewport(previousViewport); return nullptr;
			}
			FilteringMode filtering = lookup ? _preset->textures[lookupIndex].filteringMode : definition.filteringMode;
			Wrap wrap = lookup ? _preset->textures[lookupIndex].wrapMode : definition.wrapMode;
			if (!lookup && binding.texture != source) {
				// In GL, filter/wrap belong to the texture object. Pass outputs
				// keep the settings of the pass consuming them as its input,
				// even when another pass samples them through an alias.
				filtering = linear ? kFilteringModeLinear : kFilteringModeNearest;
				wrap = kWrapModeEdge;
				for (uint prior = 0; prior < i; ++prior) {
					if (binding.texture != &_passes[prior].target->texture()) continue;
					filtering = _preset->passes[prior + 1].filteringMode;
					wrap = _preset->passes[prior + 1].wrapMode;
					break;
				}
			}
			binding.linear = filtering == kFilteringModeLinear || (!lookup && !i && binding.texture == source && filtering == kFilteringModeUnspecified && linear);
			binding.wrapS = binding.wrapT = wrap;
			binding.mipmaps = !lookup && definition.mipmapInput;
			if (binding.mipmaps) _renderer.generateMipmaps(*binding.texture);
		}
		_renderer.setRenderTarget(pass.target); _renderer.setViewport(Common::Rect(width, height)); _renderer.clear(0, 0, 0, 0);
		DrawState state; state.primitive = kTriangleStrip;
		_renderer.drawProgram(*shader.program, vertices.begin(), 4, shader.vertexStride, nullptr, 0,
			shader.vertexData.empty() ? nullptr : shader.vertexData.begin(), shader.vertexData.size(),
			bindings.empty() ? nullptr : bindings.begin(), bindings.size(), state,
			shader.fragmentData.empty() ? nullptr : shader.fragmentData.begin(), shader.fragmentData.size());
		source = &pass.target->texture();
	}
	_renderer.copyTexture(_output->texture(), Common::Rect(source->width(), source->height()),
		*source, Common::Rect(source->width(), source->height()), true);
	++_frame; _current = (_current + 1) % 8;
	_renderer.setRenderTarget(previousTarget); _renderer.setViewport(previousViewport);
	return &_output->texture();
}

} } } // namespace Graphics::Metal::Retro
